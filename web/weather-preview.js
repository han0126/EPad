(() => {
  'use strict';

  class WeatherPreview {
    constructor(client) {
      this.client = client;
      this.nodes = Object.fromEntries(
        ['panel', 'form', 'code', 'max', 'min', 'canvas', 'selection',
         'state', 'screen-code', 'send', 'clear', 'error']
          .map(name => [name, document.getElementById(`weather-preview-${name}`)])
      );
      this.icons = null;
      this.loading = false;
      this.initialized = false;
      this.state = {};
      this.lastWeather = null;
      this.nodes.form.addEventListener('submit', event => {
        event.preventDefault();
        if (this.blocked || !this.icons) return;
        const maximum = this.nodes.max.valueAsNumber;
        const minimum = this.nodes.min.valueAsNumber;
        this.nodes.min.setCustomValidity(minimum > maximum ? '最低温度不能高于最高温度。' : '');
        if (!this.nodes.form.reportValidity()) return;
        this.client.post('/api/weather/preview', new URLSearchParams({
          code: this.nodes.code.value, tempMax: String(maximum), tempMin: String(minimum)
        }));
      });
      this.nodes.clear.addEventListener('click', () => {
        if (!this.blocked) this.client.post('/api/weather/preview/clear');
      });
      this.nodes.code.addEventListener('change', () => this.drawSelection());
      for (const node of [this.nodes.max, this.nodes.min]) {
        node.addEventListener('input', () => this.nodes.min.setCustomValidity(''));
      }
    }

    get errorElement() { return this.nodes.error; }

    updateControls(state) {
      this.state = state;
      this.nodes.panel.hidden = !state.isCalendar;
      this.blocked = !state.online || !state.token || state.busy || state.posting ||
        state.closed || !state.isCalendar || state.calendarBusy;
      const inputBlocked = this.blocked || !this.icons || this.loading;
      for (const node of [this.nodes.code, this.nodes.max, this.nodes.min, this.nodes.send]) {
        node.disabled = inputBlocked;
      }
      this.nodes.clear.disabled = this.blocked;
    }

    resetSession() {
      this.initialized = false;
      this.lastWeather = null;
      this.nodes.min.setCustomValidity('');
    }

    accepted() {
      this.initialized = false;
      this.lastWeather = null;
      this.nodes.state.textContent = '准备刷新';
    }

    updateStatus(weather, calendar, errorText) {
      if (!weather) return;
      this.lastWeather = weather;
      this.applyInitialSelection();
      this.nodes['screen-code'].textContent = weather.known ? String(weather.code)
        : `${weather.code} → 999`;
      this.nodes.state.textContent = calendar.state === 'error' ? (errorText || '刷新失败') :
        (calendar.state === 'queued' || calendar.state === 'drawing' ? '正在刷新' :
          (calendar.state === 'ready' ?
            (weather.preview ? '已显示天气预览' :
              ({network: '已显示网络天气', unavailable: '天气待更新', disabled: '天气未启用',
                placeholder: '静态占位天气'}[weather.source] || '等待有效数据')) : '等待有效日期'));
    }

    applyInitialSelection() {
      if (this.initialized || !this.icons || !this.lastWeather) return;
      const weather = this.lastWeather;
      // 页面重新连接时显示设备当前预览；后续轮询不覆盖用户正在编辑的选择。
      this.nodes.code.value = this.icons.has(weather.code) ? String(weather.code) : '65000';
      this.nodes.max.value = weather.temperaturesValid === false ? 28 : weather.tempMax;
      this.nodes.min.value = weather.temperaturesValid === false ? 18 : weather.tempMin;
      this.initialized = true;
      this.drawSelection();
    }

    async load() {
      if (this.icons || this.loading || this.state.closed || this.state.posting || !this.state.isCalendar) return;
      this.loading = true;
      this.client.refreshControls();
      try {
        const catalogue = await this.client.request('/weather-icons.json');
        if (catalogue.size !== 30 || catalogue.stride !== 4 || !Array.isArray(catalogue.icons)) {
          throw new Error('invalid_catalogue');
        }
        const icons = new Map();
        for (const icon of catalogue.icons) {
          if (!/^[0-9a-f]{240}$/i.test(icon.bitmap) || !Number.isInteger(icon.code) ||
              !Number.isInteger(icon.color) || icon.color < 0 || icon.color > 3) {
            throw new Error('invalid_catalogue');
          }
          icons.set(icon.code, { ...icon,
            bytes: Uint8Array.from(icon.bitmap.match(/../g), value => parseInt(value, 16))
          });
        }
        if (!icons.has(999)) throw new Error('invalid_catalogue');
        this.icons = icons;
        this.nodes.code.replaceChildren();
        for (const icon of icons.values()) {
          this.nodes.code.add(new Option(`${icon.code} · ${icon.name}`, String(icon.code)));
        }
        this.nodes.code.add(new Option('65000 · 未收录代码', '65000'));
        this.nodes.error.textContent = '';
        this.applyInitialSelection();
        this.drawSelection();
      } catch (error) {
        this.nodes.error.textContent = '内置图标加载失败，等待重新连接。';
      } finally {
        this.loading = false;
        this.client.refreshControls();
      }
    }

    drawSelection() {
      if (!this.icons) return;
      const code = Number(this.nodes.code.value);
      const icon = this.icons.get(code) || this.icons.get(999);
      this.nodes.selection.textContent = `${code} · ${icon.name}`;
      const context = this.nodes.canvas.getContext('2d');
      if (!context) {
        this.nodes.error.textContent = '浏览器无法显示本地图标预览。';
        return;
      }
      const image = context.createImageData(30, 30);
      image.data.fill(255);
      const palette = [[0, 0, 0], [255, 255, 255], [255, 255, 0], [255, 0, 0]];
      const plot = (x, y, color) => {
        if (x < 0 || x >= 30 || y < 0 || y >= 30) return;
        const offset = (y * 30 + x) * 4;
        image.data[offset] = palette[color][0];
        image.data[offset + 1] = palette[color][1];
        image.data[offset + 2] = palette[color][2];
      };
      const isPixel = (x, y) => (icon.bytes[y * 4 + (x >> 3)] & (0x80 >> (x & 7))) !== 0;
      // 与固件一致：黄色图标先画黑边，再覆盖原始黄色笔画。
      if (icon.color === 2) {
        for (let y = 0; y < 30; y++) {
          for (let x = 0; x < 30; x++) {
            if (!isPixel(x, y)) continue;
            for (let dy = -1; dy <= 1; dy++) {
              for (let dx = -1; dx <= 1; dx++) plot(x + dx, y + dy, 0);
            }
          }
        }
      }
      for (let y = 0; y < 30; y++) {
        for (let x = 0; x < 30; x++) {
          if (isPixel(x, y)) plot(x, y, icon.color);
        }
      }
      context.putImageData(image, 0, 0);
    }
  }

  window.WeatherPreview = WeatherPreview;
})();
