(() => {
  'use strict';

  const ALGORITHMS = Object.freeze(['epd-perceptual', 'epd-adaptive', 'epd-blue', 'epd-structure', 'epd-iterative']);
  const WIDTH = 400, HEIGHT = 300;
  const DEFAULT_PALETTE = [[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0]];

  // 与独立测试页使用相同的五种实验算法；浏览器和 Worker 共用，不改变屏幕编码。
  async function engine(job, emit, cancelled) {
    const width = 400, height = 300, count = width * height;
    const fs = [[1, 0, 7 / 16], [-1, 1, 3 / 16], [0, 1, 5 / 16], [1, 1, 1 / 16]];
    let lastProgress = 0;
    async function tick(fraction, label) {
      if (cancelled()) throw new Error('image_cancelled');
      lastProgress = Math.max(lastProgress, Math.min(99, Math.floor(fraction * 99)));
      emit({ progress: lastProgress, label });
      await new Promise(resolve => setTimeout(resolve, 0));
      if (cancelled()) throw new Error('image_cancelled');
    }
    const decode = value => { const v = value / 255; return v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4; };
    const table = Float64Array.from({ length: 256 }, (_, i) => decode(i));
    const inks = job.palette.map(color => color.map(decode));
    const luminance = rgb => .2126729 * rgb[0] + .7151522 * rgb[1] + .072175 * rgb[2];
    function lab(r, g, b) {
      const f = t => t > 216 / 24389 ? Math.cbrt(t) : (24389 / 27 * t + 16) / 116;
      const x = f((.4124564 * r + .3575761 * g + .1804375 * b) / .95047);
      const y = f(.2126729 * r + .7151522 * g + .072175 * b);
      const z = f((.0193339 * r + .119192 * g + .9503041 * b) / 1.08883);
      return [116 * y - 16, 500 * (x - y), 200 * (y - z)];
    }
    const inkLab = inks.map(c => lab(...c));
    const distance = (a, b) => 1.8 * (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2;
    function nearest(rgb, neutral = false) {
      const value = lab(...rgb);
      let best = 0, error = Infinity;
      for (let i = 0; i < (neutral ? 2 : 4); i += 1) {
        const d = distance(value, inkLab[i]);
        if (d < error) { error = d; best = i; }
      }
      return best;
    }
    function mix(weights) {
      const out = [0, 0, 0];
      for (let i = 0; i < 4; i += 1) for (let c = 0; c < 3; c += 1) out[c] += weights[i] * inks[i][c];
      return out;
    }
  
    let lut = null, targets = null, weights = null, neutrals = null, edges = null;
    async function prepareMapping() {
      if (targets) return;
      const mixtures = [];
      // 在固定四色的面积比例单纯形中搜索，禁止自定义出屏幕不存在的第五种墨色。
      for (let a = 0; a <= 12; a += 1) for (let b = 0; b <= 12 - a; b += 1) for (let c = 0; c <= 12 - a - b; c += 1) {
        const w = [a / 12, b / 12, c / 12, (12 - a - b - c) / 12];
        const color = mix(w);
        mixtures.push({ w, rgb: color, lab: lab(...color) });
      }
      lut = new Float32Array(17 * 17 * 17 * 4);
      for (let r = 0; r < 17; r += 1) {
        for (let g = 0; g < 17; g += 1) for (let b = 0; b < 17; b += 1) {
          const target = lab(decode(r * 255 / 16), decode(g * 255 / 16), decode(b * 255 / 16));
          let best = mixtures[0], error = Infinity;
          for (const candidate of mixtures) {
            const e = distance(target, candidate.lab);
            if (e < error) { error = e; best = candidate; }
          }
          let w = best.w.slice();
          for (const step of [1 / 24, 1 / 48, 1 / 96, 1 / 192]) {
            let improved = w;
            for (let from = 0; from < 4; from += 1) for (let to = 0; to < 4; to += 1) {
              if (from === to || w[from] < step) continue;
              const trial = w.slice(); trial[from] -= step; trial[to] += step;
              const e = distance(target, lab(...mix(trial)));
              if (e < error) { error = e; improved = trial; }
            }
            w = improved;
          }
          lut.set(w, ((r * 17 + g) * 17 + b) * 4);
        }
        await tick(r / 17 * .15, '建立感知色域映射');
      }
      weights = new Float32Array(count * 4);
      targets = new Float32Array(count * 3);
      neutrals = new Uint8Array(count);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const p = y * width + x, i = p * 4;
          if (!job.mask[p]) { weights[i + 1] = 1; targets.set(inks[1], p * 3); neutrals[p] = 1; continue; }
          const rgb = [job.pixels[i], job.pixels[i + 1], job.pixels[i + 2]];
          const targetLab = lab(table[rgb[0]], table[rgb[1]], table[rgb[2]]);
          const neutral = job.neutral && Math.hypot(targetLab[1], targetLab[2]) < 10;
          neutrals[p] = neutral ? 1 : 0;
          if (neutral) {
            let low = 0, high = 1;
            // 黑白线上的一维最小化也使用实际色板，不假定纸白是纯白。
            const evaluate = t => distance(targetLab, lab(...inks[0].map((v, c) => v * (1 - t) + inks[1][c] * t)));
            for (let j = 0; j < 16; j += 1) {
              const a = (2 * low + high) / 3, b = (low + 2 * high) / 3;
              if (evaluate(a) < evaluate(b)) high = b; else low = a;
            }
            let t = (low + high) / 2;
            const error = evaluate(t);
            if (evaluate(0) <= error) t = 0;
            if (evaluate(1) <= evaluate(t)) t = 1;
            weights[i] = 1 - t; weights[i + 1] = t;
          } else {
            const pos = rgb.map(v => v * 16 / 255), base = pos.map(v => Math.min(15, Math.floor(v)));
            const frac = pos.map((v, c) => v - base[c]);
            for (let dr = 0; dr < 2; dr += 1) for (let dg = 0; dg < 2; dg += 1) for (let db = 0; db < 2; db += 1) {
              const factor = (dr ? frac[0] : 1 - frac[0]) * (dg ? frac[1] : 1 - frac[1]) * (db ? frac[2] : 1 - frac[2]);
              const offset = (((base[0] + dr) * 17 + base[1] + dg) * 17 + base[2] + db) * 4;
              for (let c = 0; c < 4; c += 1) weights[i + c] += lut[offset + c] * factor;
            }
          }
          for (let color = 0; color < 4; color += 1) for (let c = 0; c < 3; c += 1) targets[p * 3 + c] += weights[i + color] * inks[color][c];
        }
        if ((y & 15) === 15) await tick(.15 + y / height * .1, '映射原图颜色');
      }
      edges = new Float32Array(count * 3);
      for (let y = 0; y < height; y += 1) for (let x = 0; x < width; x += 1) {
        const p = y * width + x;
        const value = (xx, yy) => {
          const i = (Math.max(0, Math.min(height - 1, yy)) * width + Math.max(0, Math.min(width - 1, xx))) * 3;
          return .2126729 * targets[i] + .7151522 * targets[i + 1] + .072175 * targets[i + 2];
        };
        const dx = (value(x + 1, y) - value(x - 1, y)) / 2, dy = (value(x, y + 1) - value(x, y - 1)) / 2;
        edges.set([dx, dy, Math.min(1, Math.hypot(dx, dy) * 5)], p * 3);
      }
    }
  
    async function diffuse(id) {
      const out = new Uint8Array(count);
      const errors = [new Float32Array(width * 3), new Float32Array(width * 3)];
      for (let y = 0; y < height; y += 1) {
        const direction = (y & 1) ? -1 : 1;
        for (let step = 0; step < width; step += 1) {
          const x = direction === 1 ? step : width - 1 - step, p = y * width + x;
          if (!job.mask[p]) { out[p] = 1; continue; }
          let rgb = [0, 1, 2].map(c => targets[p * 3 + c] + errors[0][x * 3 + c]);
          if (neutrals[p]) {
            const delta = inks[1].map((v, c) => v - inks[0][c]);
            const denominator = delta.reduce((sum, v) => sum + v * v, 0);
            const t = denominator > 1e-12 ? delta.reduce((sum, v, c) => sum + v * (rgb[c] - inks[0][c]), 0) / denominator : 0;
            rgb = inks[0].map((v, c) => v + t * delta[c]);
          }
          const best = nearest(rgb, !!neutrals[p]); out[p] = best;
          let kernel = fs;
          if (id === 'adaptive') {
            // 实验性连续变系数，不冒用论文的优化系数表或质量结论。
            const t = Math.max(0, Math.min(1, luminance(rgb))), extreme = Math.abs(t - .5) * 2;
            const forward = .40 + .20 * extreme, diagonal = .28 - .10 * extreme;
            kernel = [[1, 0, forward], [-1, 1, diagonal], [0, 1, 1 - forward - diagonal]];
          }
          let total = 0;
          const neighbors = [];
          for (const [dx, dy, weight] of kernel) {
            const xx = x + direction * dx, yy = y + dy;
            if (xx < 0 || xx >= width || yy >= height || !job.mask[yy * width + xx]) continue;
            let w = weight;
            if (id === 'structure') {
              const q = yy * width + xx, strength = edges[p * 3 + 2];
              const difference = Math.abs(luminance([targets[q * 3], targets[q * 3 + 1], targets[q * 3 + 2]]) - luminance([targets[p * 3], targets[p * 3 + 1], targets[p * 3 + 2]]));
              const norm = Math.hypot(edges[p * 3], edges[p * 3 + 1]) * Math.hypot(dx, dy);
              const crossing = norm > 1e-8 ? Math.abs(edges[p * 3] * direction * dx + edges[p * 3 + 1] * dy) / norm : 0;
              w *= Math.exp(-difference * 10) * (1 - .75 * strength * crossing);
            }
            total += w; neighbors.push([xx, dy, w]);
          }
          // 只在结构模式中归一化方向权重，保留 FS 基准原有的边缘行为。
          const normalizer = id === 'structure' && total > 1e-9 ? 1 / total : 1;
          for (const [xx, dy, weight] of neighbors) for (let c = 0; c < 3; c += 1) errors[dy][xx * 3 + c] += (rgb[c] - inks[best][c]) * weight * normalizer;
        }
        const old = errors.shift(); old.fill(0); errors.push(old);
        if ((y & 15) === 15) await tick(.25 + y / height * (id === 'iterative' ? .15 : .75), '向量误差扩散');
      }
      return out;
    }
  
    async function blueNoise() {
      const size = 64, n = size * size, pattern = new Uint8Array(n), density = new Float64Array(n), rank = new Uint16Array(n);
      let random = 0x796684;
      const rng = () => { random ^= random << 13; random ^= random >>> 17; random ^= random << 5; return (random >>> 0) / 4294967296; };
      const order = Array.from({ length: n }, (_, i) => i);
      for (let i = n - 1; i > 0; i -= 1) { const j = Math.floor(rng() * (i + 1)); [order[i], order[j]] = [order[j], order[i]]; }
      const footprint = [];
      for (let dy = -5; dy <= 5; dy += 1) for (let dx = -5; dx <= 5; dx += 1) footprint.push([dx, dy, Math.exp(-(dx * dx + dy * dy) / (2 * 1.5 * 1.5))]);
      function change(p, on) {
        pattern[p] = on ? 1 : 0;
        const x = p % size, y = Math.floor(p / size), sign = on ? 1 : -1;
        for (const [dx, dy, value] of footprint) density[((y + dy + size) % size) * size + (x + dx + size) % size] += sign * value;
      }
      function pick(on) {
        let best = -1, value = on ? -Infinity : Infinity;
        for (let i = 0; i < n; i += 1) if (pattern[i] === (on ? 1 : 0) && (on ? density[i] > value : density[i] < value)) { best = i; value = density[i]; }
        return best;
      }
      for (let i = 0; i < n / 2; i += 1) change(order[i], true);
      // 有界 Void-and-Cluster 松弛；固定种子使重复转换能直接比较。
      for (let iteration = 0; iteration < 2048; iteration += 1) {
        const cluster = pick(true); change(cluster, false);
        const empty = pick(false); change(empty, true);
        if (cluster === empty) break;
        if ((iteration & 127) === 127) await tick(.25 + iteration / 2048 * .15, '蓝噪声松弛');
      }
      const initial = pattern.slice();
      for (let value = n / 2 - 1; value >= 0; value -= 1) {
        const p = pick(true); rank[p] = value; change(p, false);
        if ((value & 127) === 0) await tick(.4 + (n / 2 - value) / n * .4, '生成蓝噪声阈值');
      }
      pattern.fill(0); density.fill(0);
      for (let p = 0; p < n; p += 1) if (initial[p]) change(p, true);
      for (let value = n / 2; value < n; value += 1) {
        const p = pick(false); rank[p] = value; change(p, true);
        if ((value & 127) === 0) await tick(.6 + (value - n / 2) / n * .4, '生成蓝噪声阈值');
      }
      const out = new Uint8Array(count);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const p = y * width + x;
          if (!job.mask[p]) { out[p] = 1; continue; }
          const threshold = (rank[(y % size) * size + x % size] + .5) / n;
          let sum = 0, best = 3;
          for (let color = 0; color < 4; color += 1) { sum += weights[p * 4 + color]; if (threshold < sum) { best = color; break; } }
          out[p] = best;
        }
        if ((y & 15) === 15) await tick(.8 + y / height * .2, '四色面积分配');
      }
      return out;
    }
  
    async function optimize(out) {
      // 两个空间尺度的视觉误差 + 较弱的边缘点误差；不宣称复现原版 Color DBS。
      const opponent = rgb => { const y = luminance(rgb); return [2 * y, .6 * (rgb[0] - y), .6 * (rgb[2] - y)]; };
      const colors = inks.map(opponent), target = new Float64Array(count * 3), residual = new Float64Array(count * 3);
      const pointWeight = new Float64Array(count);
      for (let p = 0; p < count; p += 1) {
        target.set(opponent([targets[p * 3], targets[p * 3 + 1], targets[p * 3 + 2]]), p * 3);
        for (let c = 0; c < 3; c += 1) residual[p * 3 + c] = colors[out[p]][c] - target[p * 3 + c];
        pointWeight[p] = .02 + .10 * edges[p * 3 + 2];
      }
      const models = [];
      for (const [one, factor] of [[[.25, .5, .25], .65], [[1 / 16, 4 / 16, 6 / 16, 4 / 16, 1 / 16], .35]]) {
        const radius = (one.length - 1) / 2, kernel = [];
        for (let dy = -radius; dy <= radius; dy += 1) for (let dx = -radius; dx <= radius; dx += 1) kernel.push([dx, dy, one[dx + radius] * one[dy + radius]]);
        const filtered = new Float64Array(residual.length);
        for (let y = 0; y < height; y += 1) {
          for (let x = 0; x < width; x += 1) for (const [dx, dy, k] of kernel) {
            const xx = x + dx, yy = y + dy;
            if (xx < 0 || xx >= width || yy < 0 || yy >= height) continue;
            const p = (y * width + x) * 3, q = (yy * width + xx) * 3;
            for (let c = 0; c < 3; c += 1) filtered[p + c] += residual[q + c] * k;
          }
          if ((y & 31) === 31) await tick(.4, '建立多尺度误差');
        }
        models.push({ kernel, factor, filtered, one, radius });
      }
      function gradient(p) {
        const x = p % width, y = Math.floor(p / width), g = [0, 0, 0];
        let hessian = pointWeight[p];
        for (let c = 0; c < 3; c += 1) g[c] = residual[p * 3 + c] * pointWeight[p];
        for (const model of models) for (const [dx, dy, k] of model.kernel) {
          const xx = x + dx, yy = y + dy;
          if (xx < 0 || xx >= width || yy < 0 || yy >= height) continue;
          const i = (yy * width + xx) * 3;
          hessian += model.factor * k * k;
          for (let c = 0; c < 3; c += 1) g[c] += model.factor * k * model.filtered[i + c];
        }
        return { g, hessian };
      }
      function apply(p, delta) {
        const x = p % width, y = Math.floor(p / width);
        for (let c = 0; c < 3; c += 1) residual[p * 3 + c] += delta[c];
        for (const model of models) for (const [dx, dy, k] of model.kernel) {
          const xx = x + dx, yy = y + dy;
          if (xx < 0 || xx >= width || yy < 0 || yy >= height) continue;
          const i = (yy * width + xx) * 3;
          for (let c = 0; c < 3; c += 1) model.filtered[i + c] += delta[c] * k;
        }
      }
      function cross(p, q) {
        const px = p % width, py = Math.floor(p / width), qx = q % width, qy = Math.floor(q / width);
        let value = 0;
        for (const model of models) for (const [dx, dy, k] of model.kernel) {
          const xx = px + dx, yy = py + dy, qdx = xx - qx, qdy = yy - qy;
          if (xx < 0 || xx >= width || yy < 0 || yy >= height || Math.abs(qdx) > model.radius || Math.abs(qdy) > model.radius) continue;
          value += model.factor * k * model.one[qdx + model.radius] * model.one[qdy + model.radius];
        }
        return value;
      }
      const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
      for (let pass = 0; pass < job.passes; pass += 1) {
        let changes = 0;
        for (let row = 0; row < height; row += 1) {
          const y = pass & 1 ? height - 1 - row : row, direction = (y + pass) & 1 ? -1 : 1;
          for (let step = 0; step < width; step += 1) {
            const x = direction === 1 ? step : width - 1 - step, p = y * width + x;
            if (!job.mask[p]) continue;
            const before = out[p], gp = gradient(p);
            let best = before, minimum = -1e-11, deltaBest = null;
            for (let color = 0; color < (neutrals[p] ? 2 : 4); color += 1) {
              if (color === before) continue;
              const delta = colors[color].map((v, c) => v - colors[before][c]);
              const change = 2 * dot(delta, gp.g) + dot(delta, delta) * gp.hessian;
              if (change < minimum) { minimum = change; best = color; deltaBest = delta; }
            }
            if (best !== before) { apply(p, deltaBest); out[p] = best; changes += 1; }
            // 交换保持局部墨色数量，改善只靠单点改色难以移动的颗粒。
            const xx = x + (pass & 1 ? 0 : direction), yy = y + (pass & 1 ? 1 : 0);
            if ((x & 1) || xx < 0 || xx >= width || yy >= height) continue;
            const q = yy * width + xx;
            if (!job.mask[q] || out[p] === out[q] || (neutrals[p] && out[q] > 1) || (neutrals[q] && out[p] > 1)) continue;
            const ga = gradient(p), gb = gradient(q), delta = colors[out[q]].map((v, c) => v - colors[out[p]][c]);
            const difference = ga.g.map((v, c) => v - gb.g[c]);
            const change = 2 * dot(delta, difference) + dot(delta, delta) * (ga.hessian + gb.hessian - 2 * cross(p, q));
            if (change < -1e-11) {
              apply(p, delta); apply(q, delta.map(v => -v));
              [out[p], out[q]] = [out[q], out[p]]; changes += 1;
            }
          }
          if ((row & 7) === 7) await tick(.4 + (pass + row / height) / job.passes * .6, `迭代 ${pass + 1} / ${job.passes}`);
        }
        if (changes === 0) break;
      }
      return out;
    }
  
    await tick(0, '开始转换');
    await prepareMapping();
    let output;
    if (job.algorithm === 'blue') output = await blueNoise();
    else {
      output = await diffuse(job.algorithm);
      if (job.algorithm === 'iterative') output = await optimize(output);
    }
    await tick(1, '四色像素已生成');
    return output;
  }

  async function convert(pixels, coverage, config, options = {}) {
    if (!config || !ALGORITHMS.includes(config.algorithm) ||
      !(pixels instanceof Uint8ClampedArray) || pixels.length !== WIDTH * HEIGHT * 4 ||
      !(coverage instanceof Uint8ClampedArray) || coverage.length !== pixels.length) throw new Error('image_options');
    const palette = config.palette === undefined ? DEFAULT_PALETTE : config.palette;
    const passes = config.passes === undefined ? 4 : config.passes;
    const neutral = config.neutral === undefined ? true : config.neutral;
    if (!Array.isArray(palette) || palette.length !== 4 ||
      palette.some(color => !Array.isArray(color) || color.length !== 3 ||
        color.some(value => !Number.isInteger(value) || value < 0 || value > 255)) ||
      !Number.isInteger(passes) || passes < 1 || passes > 12 || typeof neutral !== 'boolean') throw new Error('image_options');
    const light = color => .2126 * color[0] + .7152 * color[1] + .0722 * color[2];
    if (light(palette[1]) <= light(palette[0]) + 10 ||
      palette.some((color, i) => palette.some((other, j) => i !== j && color.every((v, c) => v === other[c])))) throw new Error('image_palette');
    const mask = new Uint8Array(WIDTH * HEIGHT);
    for (let p = 0; p < mask.length; p += 1) mask[p] = coverage[p * 4 + 3] !== 0 ? 1 : 0;
    const job = { pixels, mask, palette: palette.map(color => color.slice()), passes, neutral,
      algorithm: config.algorithm.slice(4) };
    return engine(job, message => {
      if (typeof options.onProgress === 'function') options.onProgress(message.progress, message.label);
    }, () => typeof options.isCurrent === 'function' && !options.isCurrent());
  }

  self.ImageAlgorithms = Object.freeze({ algorithms: ALGORITHMS, convert });
})();

