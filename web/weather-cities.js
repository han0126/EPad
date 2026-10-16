(() => {
  'use strict';

  class WeatherCities {
    constructor({ request, refreshControls, credentials, selectCity }) {
      Object.assign(this, { request, refreshControls, credentials, selectCity });
      this.elements = Object.fromEntries(['weather-city-fields', 'weather-city-query', 'weather-city-province',
        'weather-city-search', 'weather-city-results', 'weather-city-state', 'weather-city-error',
        'weather-city-sources', 'weather-region'].map(id => [id, document.getElementById(id)]));
      this.context = { blocked: true, closed: false, isCalendar: true, token: '' };
      this.searching = false;
      this.revision = 0;
      this.selectedId = '';
      this.queryId = 0;
      this.cities = new Map();
      this.elements['weather-city-search'].addEventListener('click', () => this.search());
      for (const id of ['weather-city-query', 'weather-city-province']) {
        this.elements[id].addEventListener('input', () => {
          this.clearSelection();
          this.refreshControls();
        });
        this.elements[id].addEventListener('keydown', event => {
          if (event.key !== 'Enter') return;
          event.preventDefault();
          this.search();
        });
      }
      this.elements['weather-city-results'].addEventListener('change', event => {
        const city = this.cities.get(event.target.value);
        this.selectedId = city ? city.id : '';
        if (city) {
          this.selectCity(city);
          this.elements['weather-city-state'].textContent = `${city.province} / ${city.name} · 未保存`;
        }
        this.refreshControls();
      });
      window.addEventListener('pagehide', () => this.cancel());
    }

    cancel() {
      this.revision += 1;
      this.searching = false;
    }

    clearSelection() {
      this.selectedId = '';
      this.queryId = 0;
      this.cities.clear();
      const empty = document.createElement('option');
      empty.value = '';
      empty.textContent = '请选择城市';
      this.elements['weather-city-results'].replaceChildren(empty);
      this.elements['weather-city-state'].textContent = '';
      this.elements['weather-city-error'].textContent = '';
      this.elements['weather-city-sources'].replaceChildren();
    }

    resetSession() {
      this.cancel();
      this.clearSelection();
    }

    modeChanged() {
      if (this.elements['weather-region'].value !== 'search') this.clearSelection();
    }

    applySettings(data) {
      this.cancel();
      this.clearSelection();
      if (!data.cityId) return;
      const city = { id: data.cityId, name: data.cityName, province: data.cityProvince,
        latitude100: data.latitude100, longitude100: data.longitude100 };
      this.cities.set(city.id, city);
      this.selectedId = city.id;
      this.elements['weather-city-query'].value = city.name;
      this.elements['weather-city-province'].value = city.province;
      const option = document.createElement('option');
      option.value = city.id;
      option.textContent = `${city.province} / ${city.name}`;
      this.elements['weather-city-results'].append(option);
      this.elements['weather-city-results'].value = city.id;
      this.elements['weather-city-state'].textContent = `${city.province} / ${city.name} · 已保存`;
    }

    needsSelection() {
      return this.elements['weather-region'].value === 'search' && !this.selectedId;
    }

    saveFields() {
      if (this.needsSelection()) throw new Error('weather_city_required');
      const search = this.elements['weather-region'].value === 'search';
      return { cityMode: search ? 'search' : 'coordinates', cityId: search ? this.selectedId : '',
        cityQueryId: search ? String(this.queryId) : '0' };
    }

    updateControls(context) {
      this.context = context;
      if ((context.closed || !context.isCalendar) && this.searching) this.cancel();
      const search = this.elements['weather-region'].value === 'search';
      this.elements['weather-city-fields'].hidden = !context.isCalendar || !search;
      const blocked = context.blocked || this.searching;
      for (const id of ['weather-city-query', 'weather-city-province', 'weather-city-search']) {
        this.elements[id].disabled = blocked;
      }
      this.elements['weather-city-results'].disabled = blocked || this.cities.size === 0;
      this.elements['weather-city-search'].textContent = this.searching ? '搜索中…' : '搜索城市';
    }

    render(data) {
      if (!Number.isInteger(data.id) || data.id <= 0 || data.id > 0xFFFFFFFF ||
        !Array.isArray(data.cities) || data.cities.length > 10) throw new Error('城市查询响应异常。');
      const candidates = new Map();
      for (const city of data.cities) {
        if (!city || typeof city.id !== 'string' || !/^[A-Za-z0-9]{1,23}$/.test(city.id) ||
          candidates.has(city.id) || typeof city.name !== 'string' || !city.name.trim() ||
          typeof city.province !== 'string' || !city.province.trim() ||
          !Number.isInteger(city.latitude100) || Math.abs(city.latitude100) > 9000 ||
          !Number.isInteger(city.longitude100) || Math.abs(city.longitude100) > 18000) {
          throw new Error('城市查询响应异常。');
        }
        candidates.set(city.id, city);
      }
      // 完整验证后才替换选项，异常响应不能留下部分可保存的结果。
      this.clearSelection();
      this.queryId = data.id;
      for (const city of candidates.values()) {
        this.cities.set(city.id, city);
        const option = document.createElement('option');
        option.value = city.id;
        option.textContent = `${city.province} / ${city.name}`;
        this.elements['weather-city-results'].append(option);
      }
      this.elements['weather-city-state'].textContent = data.cities.length
        ? `找到 ${data.cities.length} 个城市` : '未找到市级地区';
      for (const source of Array.isArray(data.sources) ? data.sources : []) {
        if (typeof source !== 'string' || !source) continue;
        let element = document.createElement('span');
        element.textContent = source;
        try {
          const url = new URL(source);
          if (url.protocol === 'https:' && !url.username && !url.password) {
            element = document.createElement('a');
            element.href = url.href;
            element.textContent = source;
            element.target = '_blank';
            element.rel = 'noopener noreferrer';
          }
        } catch (error) { /* 文字来源只作为文本展示。 */ }
        this.elements['weather-city-sources'].append(element);
      }
    }

    async search() {
      if (this.context.blocked || this.context.closed || this.searching ||
        this.elements['weather-region'].value !== 'search') return;
      const query = this.elements['weather-city-query'].value.trim();
      const province = this.elements['weather-city-province'].value.trim();
      if (!query) {
        this.elements['weather-city-error'].textContent = '请输入城市名称。';
        return;
      }
      const body = new URLSearchParams({ query, province, ...this.credentials() });
      const token = this.context.token;
      const revision = ++this.revision;
      const current = () => revision === this.revision && token === this.context.token &&
        !this.context.closed && this.context.isCalendar;
      this.clearSelection();
      this.searching = true;
      this.elements['weather-city-state'].textContent = '正在查询城市';
      this.refreshControls();
      try {
        // POST 只发送一次，随后仅查询本地任务状态，不重复请求 GeoAPI。
        let data = await this.request('/api/weather/cities/search', {
          method: 'POST', headers: { 'X-Portal-Token': token, 'Content-Type': 'application/x-www-form-urlencoded' }, body
        });
        const id = data.id;
        const deadline = Date.now() + 30000;
        while (current() && data.state === 'searching' && Date.now() < deadline) {
          await new Promise(resolve => setTimeout(resolve, 600));
          if (!current()) return;
          data = await this.request('/api/weather/cities/status', { headers: { 'X-Portal-Token': token } });
          if (data.id !== id) throw new Error('城市查询已被更新，请重新搜索。');
        }
        if (!current()) return;
        if (data.state === 'searching') throw new Error('城市查询超时，请确认设备状态后重试。');
        if (data.state === 'error') {
          const messages = { connection: '城市查询连接失败。', http: '城市查询被拒绝，请核对 Host、KEY、GeoAPI 权限及额度。',
            too_large: '城市响应超过容量限制。', encoding: '城市响应解压失败。', json: '城市响应格式无效。',
            forecast: '城市返回数据无效。', attribution: '城市来源声明无效。', memory: '城市查询内存不足。', task: '城市查询任务无法启动。' };
          throw new Error(`${messages[data.error] || '城市查询失败。'} HTTP ${data.httpStatus || '--'} / API ${data.apiCode || '--'}`);
        }
        if (data.state !== 'ready') throw new Error('城市查询状态异常，请重新搜索。');
        this.render(data);
      } catch (error) {
        if (!current()) return;
        this.clearSelection();
        this.elements['weather-city-state'].textContent = '查询未完成';
        this.elements['weather-city-error'].textContent = error.name === 'AbortError'
          ? '设备响应超时，未自动重发查询。' : error.message;
      } finally {
        if (revision === this.revision) this.searching = false;
        this.refreshControls();
      }
    }
  }

  window.WeatherCities = WeatherCities;
})();
