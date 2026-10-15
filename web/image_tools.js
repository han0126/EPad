(() => {
  'use strict';

  const WIDTH = 400;
  const HEIGHT = 300;
  const FRAME_BYTES = WIDTH * HEIGHT / 4;
  // 编码顺序与 JD79668 一致：黑=0、白=1、黄=2、红=3。
  const PALETTE = [[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0]];
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

  function loadImage(file) {
    const types = ['image/png', 'image/jpeg', 'image/webp'];
    if (!file || !file.size || (file.type ? !types.includes(file.type) :
      !/\.(png|jpe?g|webp)$/i.test(file.name))) {
      return Promise.reject(new Error('image_format'));
    }
    if (file.size > 10 * 1024 * 1024) return Promise.reject(new Error('image_too_large'));
    return new Promise((resolve, reject) => {
      const image = new Image();
      const url = URL.createObjectURL(file);
      const cleanup = () => {
        clearTimeout(timer);
        image.onload = null;
        image.onerror = null;
        URL.revokeObjectURL(url);
      };
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

  function quantize(imageData, dither) {
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
          (dither ? currentErrors[errorOffset + channel] : 0)));
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
          if (dither) {
            // Floyd-Steinberg 误差扩散，只保留相邻两行的误差。
            const error = (rgb[channel] - output) / 16;
            currentErrors[errorOffset + 3 + channel] += error * 7;
            nextErrors[errorOffset - 3 + channel] += error * 3;
            nextErrors[errorOffset + channel] += error * 5;
            nextErrors[errorOffset + 3 + channel] += error;
          }
        }
        pixels[offset + 3] = 255;
      }
      const previous = currentErrors;
      currentErrors = nextErrors;
      nextErrors = previous;
      nextErrors.fill(0);
    }
    return frame;
  }

  async function prepare(file, options) {
    const image = await loadImage(file);
    try {
      const width = image.naturalWidth;
      const height = image.naturalHeight;
      if (!width || !height || width * height > 40000000) throw new Error('image_dimensions');
      const rotation = Number(options.rotation);
      if (![0, 90, 180, 270].includes(rotation)) throw new Error('image_dimensions');
      const sideways = rotation === 90 || rotation === 270;
      const orientedWidth = sideways ? height : width;
      const orientedHeight = sideways ? width : height;
      const scale = options.fit === 'cover'
        ? Math.max(WIDTH / orientedWidth, HEIGHT / orientedHeight)
        : Math.min(WIDTH / orientedWidth, HEIGHT / orientedHeight);
      const canvas = document.createElement('canvas');
      canvas.width = WIDTH;
      canvas.height = HEIGHT;
      const context = canvas.getContext('2d', { willReadFrequently: true });
      if (!context) throw new Error('image_decode');
      // 透明区域和完整显示产生的留白统一合成为白色。
      context.fillStyle = '#fff';
      context.fillRect(0, 0, WIDTH, HEIGHT);
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = 'high';
      context.save();
      context.translate(WIDTH / 2, HEIGHT / 2);
      context.rotate(rotation * Math.PI / 180);
      context.drawImage(image, -width * scale / 2, -height * scale / 2,
        width * scale, height * scale);
      context.restore();
      const preview = context.getImageData(0, 0, WIDTH, HEIGHT);
      const frame = quantize(preview, options.dither);
      return { frame, preview };
    } finally {
      image.src = '';
    }
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

  window.ImageTools = Object.freeze({ prepare, packet, upload });
})();
