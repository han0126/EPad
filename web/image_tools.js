(() => {
  'use strict';

  const WIDTH = 400;
  const HEIGHT = 300;
  const FRAME_BYTES = WIDTH * HEIGHT / 4;
  const MAX_PIXELS = 16000000;
  const MAX_SIDE = 8192;
  const WORKER_CONTEXT = typeof document === 'undefined';
  // 同一脚本在页面和 Worker 中共用算法，避免两套转换结果不一致。
  const SCRIPT_URL = WORKER_CONTEXT ? '' : (document.currentScript ? document.currentScript.src : '');
  // 编码顺序与 JD79668 一致：黑=0、白=1、黄=2、红=3。
  const PALETTE = [[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0]];
  const FLOYD = [[1, 0, 7 / 16], [-1, 1, 3 / 16], [0, 1, 5 / 16], [1, 1, 1 / 16]];
  // JJN 把完整误差分布到后续两行，和 FS 共用固定四色映射。
  // 核权重参考：https://tannerhelland.com/2012/12/28/dithering-eleven-algorithms-source-code.html
  const JJN = [[1, 0, 7 / 48], [2, 0, 5 / 48],
    [-2, 1, 3 / 48], [-1, 1, 5 / 48], [0, 1, 7 / 48], [1, 1, 5 / 48], [2, 1, 3 / 48],
    [-2, 2, 1 / 48], [-1, 2, 3 / 48], [0, 2, 5 / 48], [1, 2, 3 / 48], [2, 2, 1 / 48]];
  const RED_WEIGHT = 0.299;
  const GREEN_WEIGHT = 0.587;
  const BLUE_WEIGHT = 0.114;
  let sourceFile = null;
  let sourceImage = null;
  let sourcePromise = null;
  let sourceRevision = 0;
  let pendingLoadCancel = null;
  let processingRevision = 0;
  let worker = null;
  let workerTask = null;
  let workerDisabled = false;
  const crcTable = new Uint32Array(256);
  for (let i = 0; i < crcTable.length; i += 1) {
    let value = i;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value >>> 1) ^ ((value & 1) ? 0xEDB88320 : 0);
    }
    crcTable[i] = value >>> 0;
  }

  function crc32(bytes) {
    let value = 0xFFFFFFFF;
    for (const byte of bytes) value = (value >>> 8) ^ crcTable[(value ^ byte) & 255];
    return (value ^ 0xFFFFFFFF) >>> 0;
  }

  function validateDimensions(width, height) {
    if (!width || !height || width > MAX_SIDE || height > MAX_SIDE || width * height > MAX_PIXELS) {
      throw new Error('image_dimensions');
    }
  }

  function imageDimensions(buffer) {
    const bytes = new Uint8Array(buffer);
    const view = new DataView(buffer);
    const matches = (offset, values) => offset + values.length <= bytes.length &&
      values.every((value, index) => bytes[offset + index] === value);
    if (bytes.length >= 33 && matches(0, [137, 80, 78, 71, 13, 10, 26, 10]) &&
      view.getUint32(8) === 13 && matches(12, [73, 72, 68, 82])) {
      return [view.getUint32(16), view.getUint32(20)];
    }
    if (matches(0, [255, 216])) {
      // JPEG 尺寸在 SOF 段中；跳过元数据，不进入压缩像素区。
      let offset = 2;
      while (offset < bytes.length) {
        if (bytes[offset++] !== 255) break;
        while (offset < bytes.length && bytes[offset] === 255) offset += 1;
        if (offset >= bytes.length) break;
        const marker = bytes[offset++];
        if (marker === 0xDA || marker === 0xD9 || marker === 0) break;
        if (marker === 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (offset + 2 > bytes.length) break;
        const length = view.getUint16(offset);
        if (length < 2 || offset + length > bytes.length) break;
        if (marker >= 0xC0 && marker <= 0xCF && ![0xC4, 0xC8, 0xCC].includes(marker)) {
          if (length < 8) break;
          return [view.getUint16(offset + 5), view.getUint16(offset + 3)];
        }
        offset += length;
      }
    }
    if (bytes.length >= 20 && matches(0, [82, 73, 70, 70]) && matches(8, [87, 69, 66, 80])) {
      const end = view.getUint32(4, true) + 8;
      if (end > bytes.length) throw new Error('image_decode');
      const uint24 = offset => bytes[offset] + bytes[offset + 1] * 256 + bytes[offset + 2] * 65536;
      for (let offset = 12; offset + 8 <= end;) {
        const length = view.getUint32(offset + 4, true);
        const start = offset + 8;
        if (start + length > end) break;
        if (matches(offset, [86, 80, 56, 88]) && length >= 10) {
          return [uint24(start + 4) + 1, uint24(start + 7) + 1];
        }
        if (matches(offset, [86, 80, 56, 76]) && length >= 5 && bytes[start] === 47) {
          const bits = view.getUint32(start + 1, true);
          return [(bits & 0x3FFF) + 1, ((bits >>> 14) & 0x3FFF) + 1];
        }
        if (matches(offset, [86, 80, 56, 32]) && length >= 10 &&
          matches(start + 3, [157, 1, 42])) {
          return [view.getUint16(start + 6, true) & 0x3FFF, view.getUint16(start + 8, true) & 0x3FFF];
        }
        offset = start + length + (length & 1);
      }
    }
    throw new Error('image_decode');
  }

  function inspectImage(file) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      let settled = false;
      let timer = null;
      const finish = (error, dimensions) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        reader.onload = reader.onerror = reader.onabort = null;
        if (pendingLoadCancel === cancelRead) pendingLoadCancel = null;
        if (error) reject(error);
        else resolve(dimensions);
      };
      const cancelRead = () => {
        finish(new Error('image_cancelled'));
        reader.abort();
      };
      pendingLoadCancel = cancelRead;
      timer = setTimeout(() => {
        finish(new Error('image_decode'));
        reader.abort();
      }, 15000);
      reader.onload = () => {
        try {
          const dimensions = imageDimensions(reader.result);
          validateDimensions(...dimensions);
          finish(null, dimensions);
        } catch (error) { finish(error); }
      };
      reader.onerror = () => finish(new Error('image_decode'));
      reader.onabort = () => finish(new Error('image_cancelled'));
      // 压缩文件最多 10 MiB；解码前先读取尺寸，避免先分配超大像素缓冲。
      try { reader.readAsArrayBuffer(file); }
      catch (error) { finish(new Error('image_decode')); }
    });
  }

  async function loadImage(file, revision) {
    const types = ['image/png', 'image/jpeg', 'image/webp'];
    if (!file || !file.size || (file.type ? !types.includes(file.type) :
      !/\.(png|jpe?g|webp)$/i.test(file.name))) {
      throw new Error('image_format');
    }
    if (file.size > 10 * 1024 * 1024) throw new Error('image_too_large');
    await inspectImage(file);
    if (revision !== sourceRevision) throw new Error('image_cancelled');
    return new Promise((resolve, reject) => {
      const image = new Image();
      const url = URL.createObjectURL(file);
      const cleanup = () => {
        clearTimeout(timer);
        image.onload = null;
        image.onerror = null;
        URL.revokeObjectURL(url);
        if (pendingLoadCancel === cancelDecode) pendingLoadCancel = null;
      };
      const cancelDecode = () => {
        cleanup();
        image.src = '';
        reject(new Error('image_cancelled'));
      };
      pendingLoadCancel = cancelDecode;
      const timer = setTimeout(() => {
        cleanup();
        image.src = '';
        reject(new Error('image_decode'));
      }, 15000);
      image.decoding = 'async';
      image.onload = () => { cleanup(); resolve(image); };
      image.onerror = () => { cleanup(); reject(new Error('image_decode')); };
      image.src = url;
    });
  }

  function releaseSource() {
    sourceRevision += 1;
    if (pendingLoadCancel) pendingLoadCancel();
    if (sourceImage) sourceImage.src = '';
    sourceFile = null;
    sourceImage = null;
    sourcePromise = null;
  }

  function release() {
    cancel();
    releaseSource();
    stopWorker(new Error('image_cancelled'));
  }

  function getImage(file) {
    if (sourceFile === file && sourcePromise) return sourcePromise;
    releaseSource();
    const revision = sourceRevision;
    sourceFile = file;
    sourcePromise = loadImage(file, revision).then(image => {
      if (revision !== sourceRevision) {
        image.src = '';
        throw new Error('image_cancelled');
      }
      try { validateDimensions(image.naturalWidth, image.naturalHeight); }
      catch (error) {
        image.src = '';
        throw error;
      }
      sourceImage = image;
      return image;
    }).catch(error => {
      if (revision === sourceRevision) releaseSource();
      throw error;
    });
    return sourcePromise;
  }

  function normalizeOptions(options) {
    const fit = options.fit === undefined ? 'contain' : options.fit;
    const rotation = Number(options.rotation === undefined ? 0 : options.rotation);
    if (!['contain', 'cover', 'manual'].includes(fit) || ![0, 90, 180, 270].includes(rotation)) throw new Error('image_options');
    const algorithm = options.algorithm === undefined ? 'legacy' : options.algorithm;
    if (!['epd-photo', 'epd-smooth', 'legacy'].includes(algorithm)) throw new Error('image_options');
    let crop = null;
    if (fit === 'manual') {
      const rect = options.crop;
      if (!rect || !['x', 'y', 'width', 'height'].every(key => Number.isFinite(rect[key])) ||
        rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > 1.000001 || rect.y + rect.height > 1.000001) {
        throw new Error('image_options');
      }
      crop = { x: rect.x, y: rect.y, width: rect.width, height: rect.height };
    }
    return { fit, rotation, algorithm, crop };
  }

  function ensureCurrent(options) {
    if (typeof options.isCurrent === 'function' && !options.isCurrent()) throw new Error('image_cancelled');
  }

  function stopWorker(error) {
    const task = workerTask;
    workerTask = null;
    if (worker) {
      worker.terminate();
      worker = null;
    }
    if (task) {
      clearTimeout(task.timer);
      task.reject(error);
    }
  }

  function cancel(file) {
    processingRevision += 1;
    if (workerTask) stopWorker(new Error('image_cancelled'));
    // 换图时立即终止旧解码；仅改变算法或选区时继续复用原图。
    if (arguments.length && file !== sourceFile) releaseSource();
  }

  function quantizeInWorker(preview, coverage, config, options) {
    return new Promise((resolve, reject) => {
      try {
        if (!worker) worker = new Worker(SCRIPT_URL);
      } catch (error) {
        reject(new Error('image_worker'));
        return;
      }
      const active = worker;
      const task = { reject, timer: null, id: processingRevision };
      workerTask = task;
      const resetTimeout = () => {
        clearTimeout(task.timer);
        task.timer = setTimeout(() => {
          if (workerTask === task) stopWorker(new Error('image_worker'));
        }, 20000);
      };
      active.onmessage = event => {
        if (workerTask !== task) return;
        if (!options.isCurrent()) { stopWorker(new Error('image_cancelled')); return; }
        const data = event.data;
        if (!data || data.id !== task.id) return;
        if (Number.isFinite(data.progress)) {
          resetTimeout();
          if (options.onProgress) options.onProgress(data.progress);
          return;
        }
        if (data.error || !(data.frame instanceof Uint8Array) || data.frame.length !== FRAME_BYTES ||
          !(data.pixels instanceof Uint8ClampedArray) || data.pixels.length !== WIDTH * HEIGHT * 4) {
          stopWorker(new Error('image_worker'));
          return;
        }
        clearTimeout(task.timer);
        workerTask = null;
        active.onmessage = active.onerror = active.onmessageerror = null;
        preview.data.set(data.pixels);
        resolve(data.frame);
      };
      active.onerror = event => {
        event.preventDefault();
        if (workerTask === task) stopWorker(new Error('image_worker'));
      };
      active.onmessageerror = () => {
        if (workerTask === task) stopWorker(new Error('image_worker'));
      };
      resetTimeout();
      try {
        // 仅传递屏幕大小的像素副本；原缓冲保留给不支持 Worker 的兼容回退。
        const pixels = new Uint8ClampedArray(preview.data);
        const mask = coverage ? new Uint8ClampedArray(coverage) : null;
        const transfers = [pixels.buffer];
        if (mask) transfers.push(mask.buffer);
        active.postMessage({ id: task.id, pixels, coverage: mask, config }, transfers);
      } catch (error) { stopWorker(new Error('image_worker')); }
    });
  }

  async function convert(preview, coverage, config, options) {
    ensureCurrent(options);
    if (!workerDisabled && typeof Worker === 'function' && SCRIPT_URL &&
      (location.protocol === 'http:' || location.protocol === 'https:')) {
      try { return await quantizeInWorker(preview, coverage, config, options); }
      catch (error) {
        if (error.message === 'image_cancelled') throw error;
        // 热点内置浏览器可能禁用 Worker；回退仍执行同一算法。
        workerDisabled = true;
        ensureCurrent(options);
      }
    }
    return quantize(preview, coverage, config, options);
  }

  async function yieldRows(y, options) {
    if ((y & 15) !== 15) return;
    if (!WORKER_CONTEXT) await new Promise(resolve => setTimeout(resolve, 0));
    ensureCurrent(options);
    if (options.onProgress) options.onProgress(Math.min(99, Math.floor((y + 1) * 100 / HEIGHT)));
  }

  async function quantizeLegacy(imageData, options) {
    const pixels = imageData.data;
    const frame = new Uint8Array(FRAME_BYTES);
    let currentErrors = new Float32Array((WIDTH + 2) * 3);
    let nextErrors = new Float32Array(currentErrors.length);
    const clamp = value => Math.max(0, Math.min(255, value));
    for (let y = 0; y < HEIGHT; y += 1) {
      for (let x = 0; x < WIDTH; x += 1) {
        const pixel = y * WIDTH + x;
        const offset = pixel * 4;
        const errorOffset = (x + 1) * 3;
        const rgb = [0, 1, 2].map(channel => clamp(pixels[offset + channel] +
          currentErrors[errorOffset + channel]));
        let nearest = 0;
        let minimum = Infinity;
        for (let color = 0; color < PALETTE.length; color += 1) {
          const red = rgb[0] - PALETTE[color][0];
          const green = rgb[1] - PALETTE[color][1];
          const blue = rgb[2] - PALETTE[color][2];
          const distance = 0.299 * red * red + 0.587 * green * green + 0.114 * blue * blue;
          if (distance < minimum) { minimum = distance; nearest = color; }
        }
        frame[pixel >> 2] |= nearest << ((3 - (x & 3)) * 2);
        for (let channel = 0; channel < 3; channel += 1) {
          const output = PALETTE[nearest][channel];
          pixels[offset + channel] = output;
          // Floyd-Steinberg 误差扩散，只保留相邻两行的误差。
          const error = (rgb[channel] - output) / 16;
          currentErrors[errorOffset + 3 + channel] += error * 7;
          nextErrors[errorOffset - 3 + channel] += error * 3;
          nextErrors[errorOffset + channel] += error * 5;
          nextErrors[errorOffset + 3 + channel] += error;
        }
        pixels[offset + 3] = 255;
      }
      const previous = currentErrors;
      currentErrors = nextErrors;
      nextErrors = previous;
      nextErrors.fill(0);
      await yieldRows(y, options);
    }
    return frame;
  }

  function nearestColor(red, green, blue) {
    let nearest = 0, minimum = Infinity;
    for (let i = 0; i < PALETTE.length; i += 1) {
      const r = red - PALETTE[i][0], g = green - PALETTE[i][1], b = blue - PALETTE[i][2];
      const error = RED_WEIGHT * r * r + GREEN_WEIGHT * g * g + BLUE_WEIGHT * b * b;
      if (error < minimum) { minimum = error; nearest = i; }
    }
    return nearest;
  }

  function mapToFourColor(red, green, blue, output) {
    const luminance = RED_WEIGHT * red + GREEN_WEIGHT * green + BLUE_WEIGHT * blue;
    // 四色混色满足 R >= G >= B。加权合并违反顺序的通道，保留加权明暗。
    // 这样蓝灰雪地映射为灰度，不让不可表示的蓝色误差污染整幅图。
    if (red < green) {
      const merged = (RED_WEIGHT * red + GREEN_WEIGHT * green) / (RED_WEIGHT + GREEN_WEIGHT);
      if (merged < blue) red = green = blue = luminance;
      else red = green = merged;
    } else if (green < blue) {
      const merged = (GREEN_WEIGHT * green + BLUE_WEIGHT * blue) / (GREEN_WEIGHT + BLUE_WEIGHT);
      if (red < merged) red = green = blue = luminance;
      else green = blue = merged;
    }
    // 抑制低色度区域的微小色偏，避免灰色和 JPEG 噪声变成红黄杂点。
    const neutral = red - blue < 12;
    if (neutral) red = green = blue = luminance;
    output[0] = red;
    output[1] = green;
    output[2] = blue;
    return neutral;
  }

  async function quantize(imageData, coverage, config, options) {
    if (config.algorithm === 'legacy') {
      return quantizeLegacy(imageData, options);
    }
    const pixels = imageData.data;
    const frame = new Uint8Array(FRAME_BYTES);
    const kernel = config.algorithm === 'epd-smooth' ? JJN : FLOYD;
    const errors = Array.from({ length: 3 }, () => new Float32Array(WIDTH * 3));
    const target = [0, 0, 0], corrected = [0, 0, 0];
    for (let y = 0; y < HEIGHT; y += 1) {
      const direction = (y & 1) ? -1 : 1;
      for (let step = 0; step < WIDTH; step += 1) {
        const x = direction === 1 ? step : WIDTH - 1 - step;
        const pixel = y * WIDTH + x;
        const offset = pixel * 4;
        let nearest = 1;
        if (coverage[offset + 3] !== 0) {
          const neutral = mapToFourColor(pixels[offset], pixels[offset + 1], pixels[offset + 2], target);
          for (let channel = 0; channel < 3; channel += 1) {
            corrected[channel] = target[channel] + errors[0][x * 3 + channel];
          }
          if (neutral) {
            // 灰度区只使用黑白，彩色区扩散过来的误差仅保留明暗分量。
            const gray = RED_WEIGHT * corrected[0] + GREEN_WEIGHT * corrected[1] + BLUE_WEIGHT * corrected[2];
            corrected[0] = corrected[1] = corrected[2] = gray;
            nearest = gray > 127.5 ? 1 : 0;
          } else {
            nearest = nearestColor(...corrected);
          }
          // 保留完整浮点误差，不先截断到 0..255，避免暗部出现系统性色偏。
          for (const [dx, dy, weight] of kernel) {
            const nextX = x + direction * dx;
            if (nextX < 0 || nextX >= WIDTH || y + dy >= HEIGHT) continue;
            for (let channel = 0; channel < 3; channel += 1) {
              errors[dy][nextX * 3 + channel] += (corrected[channel] - PALETTE[nearest][channel]) * weight;
            }
          }
        }
        frame[pixel >> 2] |= nearest << ((3 - (x & 3)) * 2);
        for (let channel = 0; channel < 3; channel += 1) pixels[offset + channel] = PALETTE[nearest][channel];
        pixels[offset + 3] = 255;
      }
      const previous = errors.shift();
      previous.fill(0);
      errors.push(previous);
      await yieldRows(y, options);
    }
    return frame;
  }

  async function prepare(file, options = {}) {
    const config = normalizeOptions(options);
    cancel();
    const imagePromise = getImage(file);
    const revision = processingRevision;
    const originalOptions = options;
    const isCurrent = () => revision === processingRevision &&
      (typeof originalOptions.isCurrent !== 'function' || originalOptions.isCurrent());
    options = {
      isCurrent,
      onProgress: value => {
        if (isCurrent() && typeof originalOptions.onProgress === 'function') originalOptions.onProgress(value);
      }
    };
    const image = await imagePromise;
    ensureCurrent(options);
    const width = image.naturalWidth;
    const height = image.naturalHeight;
    validateDimensions(width, height);
    const sideways = config.rotation === 90 || config.rotation === 270;
    const orientedWidth = sideways ? height : width;
    const orientedHeight = sideways ? width : height;
    const scale = config.fit === 'cover'
      ? Math.max(WIDTH / orientedWidth, HEIGHT / orientedHeight)
      : Math.min(WIDTH / orientedWidth, HEIGHT / orientedHeight);
    const canvas = document.createElement('canvas');
    canvas.width = WIDTH;
    canvas.height = HEIGHT;
    const context = canvas.getContext('2d', { willReadFrequently: true });
    if (!context) throw new Error('image_decode');
    const legacy = config.algorithm === 'legacy';
    if (legacy) {
      // 兼容分支保持原流程：先铺白底，再缩放绘制原图。
      context.fillStyle = '#fff';
      context.fillRect(0, 0, WIDTH, HEIGHT);
    }
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    context.save();
    if (config.crop) {
      const cropWidth = config.crop.width * orientedWidth;
      const cropHeight = config.crop.height * orientedHeight;
      // 选区位于旋转后的原图坐标中，直接绘制到小画布，不分配整张旋转画布。
      context.scale(WIDTH / cropWidth, HEIGHT / cropHeight);
      context.translate(-config.crop.x * orientedWidth, -config.crop.y * orientedHeight);
      context.translate(orientedWidth / 2, orientedHeight / 2);
      context.rotate(config.rotation * Math.PI / 180);
      context.drawImage(image, -width / 2, -height / 2, width, height);
    } else {
      context.translate(WIDTH / 2, HEIGHT / 2);
      context.rotate(config.rotation * Math.PI / 180);
      context.drawImage(image, -width * scale / 2, -height * scale / 2,
        width * scale, height * scale);
    }
    context.restore();
    const coverage = legacy ? null : context.getImageData(0, 0, WIDTH, HEIGHT).data;
    if (!legacy) {
      context.globalCompositeOperation = 'destination-over';
      context.fillStyle = '#fff';
      context.fillRect(0, 0, WIDTH, HEIGHT);
    }
    const preview = context.getImageData(0, 0, WIDTH, HEIGHT);
    const frame = await convert(preview, coverage, config, options);
    ensureCurrent(options);
    options.onProgress(100);
    return { frame, preview };
  }

  async function editorSource(file, options = {}) {
    const imagePromise = getImage(file);
    const revision = sourceRevision;
    const image = await imagePromise;
    ensureCurrent(options);
    if (revision !== sourceRevision) throw new Error('image_cancelled');
    const scale = Math.min(1, 1024 / Math.max(image.naturalWidth, image.naturalHeight));
    const canvas = document.createElement('canvas');
    canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
    canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
    const context = canvas.getContext('2d');
    if (!context) throw new Error('image_decode');
    context.imageSmoothingEnabled = true;
    context.imageSmoothingQuality = 'high';
    context.drawImage(image, 0, 0, canvas.width, canvas.height);
    return canvas;
  }

  function packet(frame) {
    if (!(frame instanceof Uint8Array) || frame.length !== FRAME_BYTES) {
      throw new Error('image_size');
    }
    const data = new Uint8Array(16 + FRAME_BYTES);
    data.set([0x45, 0x50, 0x44, 0x34]);
    const view = new DataView(data.buffer);
    view.setUint16(4, WIDTH, false);
    view.setUint16(6, HEIGHT, false);
    view.setUint32(8, FRAME_BYTES, false);
    view.setUint32(12, crc32(frame), false);
    data.set(frame, 16);
    return data;
  }

  function upload(data, token, onProgress) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/image');
      xhr.responseType = 'json';
      xhr.timeout = 20000;
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.setRequestHeader('X-Portal-Token', token);
      xhr.upload.onprogress = event => {
        if (event.lengthComputable) onProgress(Math.min(100, Math.round(event.loaded * 100 / event.total)));
      };
      xhr.onload = () => {
        const response = xhr.response;
        if (xhr.status >= 200 && xhr.status < 300 && response && response.accepted) resolve(response);
        else reject(new Error(response && response.error ? response.error : 'image_response'));
      };
      xhr.onerror = () => reject(new Error('image_network'));
      xhr.ontimeout = () => reject(new Error('image_timeout'));
      xhr.onabort = () => reject(new Error('image_aborted'));
      xhr.send(data);
    });
  }

  if (WORKER_CONTEXT) {
    self.onmessage = async event => {
      const data = event.data;
      try {
        if (!data || !(data.pixels instanceof Uint8ClampedArray) || data.pixels.length !== WIDTH * HEIGHT * 4) {
          throw new Error('image_worker');
        }
        const config = normalizeOptions(data.config || {});
        if (config.algorithm !== 'legacy' &&
          (!(data.coverage instanceof Uint8ClampedArray) || data.coverage.length !== data.pixels.length)) {
          throw new Error('image_worker');
        }
        const preview = { data: data.pixels };
        const frame = await quantize(preview, data.coverage, config, {
          onProgress: progress => self.postMessage({ id: data.id, progress })
        });
        self.postMessage({ id: data.id, frame, pixels: preview.data }, [frame.buffer, preview.data.buffer]);
      } catch (error) {
        self.postMessage({ id: data ? data.id : null, error: 'image_worker' });
      }
    };
    return;
  }

  window.ImageTools = Object.freeze({ prepare, packet, upload, release, cancel, editorSource });
})();
