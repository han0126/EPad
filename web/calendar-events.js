(() => {
  'use strict';

  class CalendarEvents {
    constructor(client) {
      this.client = client;
      this.nodes = Object.fromEntries(
        ['panel', 'form', 'kind', 'date', 'text', 'characters', 'count', 'state',
         'list', 'empty', 'save', 'cancel', 'reload', 'error']
          .map(name => [name, document.getElementById(`events-${name}`)])
      );
      this.loaded = false;
      this.loading = false;
      this.ready = false;
      this.revision = 0;
      this.capacity = 64;
      this.entries = [];
      this.editingId = 0;
      this.draftRevision = 0;
      this.dirty = false;
      this.token = '';
      this.generation = 0;
      this.blocked = true;
      this.canRead = false;
      this.canLoad = false;
      this.currentDate = '';
      this.nodes.form.addEventListener('submit', event => {
        event.preventDefault();
        this.submit();
      });
      for (const name of ['kind', 'date', 'text']) {
        this.nodes[name].addEventListener('input', () => {
          if (!this.dirty && !this.editingId) this.draftRevision = this.revision;
          this.dirty = true;
          this.updateCharacters();
          this.client.refreshControls();
        });
      }
      this.nodes.cancel.addEventListener('click', () => {
        this.clearForm();
        this.nodes.error.textContent = '';
        this.nodes.state.textContent = '新增记录';
        this.client.refreshControls();
      });
      this.nodes.reload.addEventListener('click', () => {
        this.loaded = false;
        this.load();
      });
      this.updateCharacters();
    }

    get errorElement() { return this.nodes.error; }

    resetSession() {
      // 旧会话的异步响应和编辑内容都不能进入新设备会话。
      this.generation += 1;
      this.loaded = false;
      this.loading = false;
      this.ready = false;
      this.revision = 0;
      this.entries = [];
      this.currentDate = '';
      this.token = '';
      this.clearForm();
      this.nodes.list.replaceChildren();
      this.nodes.count.textContent = '—';
      this.nodes.empty.hidden = false;
      this.nodes.state.textContent = '等待读取';
      this.nodes.error.textContent = '';
    }

    clearForm() {
      this.editingId = 0;
      this.draftRevision = this.revision;
      this.dirty = false;
      this.nodes.kind.value = 'important';
      this.nodes.date.value = this.currentDate;
      this.nodes.text.value = '';
      this.updateCharacters();
    }

    updateCharacters() {
      const count = Array.from(this.nodes.text.value).length;
      this.nodes.characters.textContent = `${count} / 15`;
      this.nodes.text.setCustomValidity(count > 15 ? '正文最多15个字符。' :
        (this.nodes.text.value.length > 0 && !this.nodes.text.value.trim() ? '正文不能全为空格。' : ''));
    }

    updateControls(context) {
      if (this.token && context.token && this.token !== context.token) this.resetSession();
      this.token = context.token;
      this.canRead = context.online && !!context.token && !context.closed && context.isCalendar;
      this.canLoad = this.canRead && !context.busy && !context.posting && !context.calendarBusy;
      this.blocked = !this.canLoad || this.loading || !this.loaded || !this.ready;
      this.nodes.panel.hidden = !context.isCalendar;
      for (const name of ['kind', 'date', 'text']) this.nodes[name].disabled = this.blocked;
      this.nodes.save.disabled = this.blocked || (!this.editingId && this.entries.length >= this.capacity);
      this.nodes.save.textContent = this.editingId ? '保存修改' : '添加记录';
      this.nodes.cancel.disabled = !this.canLoad || this.loading;
      this.nodes.cancel.textContent = this.editingId ? '取消编辑' : '清空';
      this.nodes.reload.disabled = !this.canLoad || this.loading;
      for (const button of this.nodes.list.querySelectorAll('button')) button.disabled = this.blocked;
    }

    updateStatus(data, date, validTime) {
      if (validTime) {
        this.currentDate = date;
        if (!this.nodes.date.value && !this.editingId && !this.dirty) this.nodes.date.value = date;
      }
      if (!data) return;
      if (this.loaded && data.revision !== this.revision) this.loaded = false;
      if (data.ready === false) {
        this.ready = false;
        this.nodes.state.textContent = '存储不可用';
        this.nodes.error.textContent = '记录读取或保存异常，未覆盖原数据。请复位后重新读取。';
      }
    }

    apply(data, resetForm = false) {
      if (!data || !Number.isInteger(data.revision) || !Array.isArray(data.entries) ||
          !Number.isInteger(data.capacity) || data.capacity < 1 || data.capacity > 64 ||
          data.entries.length > data.capacity || data.entries.some(item =>
            !Number.isInteger(item.id) || item.id < 1 || !['important', 'memo'].includes(item.kind) ||
            typeof item.date !== 'string' || typeof item.text !== 'string')) {
        throw new Error('设备记录响应无效，请重新读取。');
      }
      const changed = this.revision !== data.revision && (this.loaded || this.editingId || this.dirty);
      this.entries = data.entries;
      this.revision = data.revision;
      this.capacity = data.capacity;
      this.ready = data.ready === true;
      this.loaded = true;
      if (resetForm) this.clearForm();
      else if (!this.editingId && !this.dirty) this.draftRevision = this.revision;
      this.nodes.count.textContent = `${this.entries.length} / ${this.capacity}`;
      this.nodes.state.textContent = this.ready ? '已读取设备记录' : '存储不可用';
      if (changed && (this.editingId || this.dirty)) {
        this.nodes.state.textContent = '记录已更新，请重新选择待编辑记录';
      }
      if (!this.ready) this.nodes.error.textContent = '记录读取或保存异常，未覆盖原数据。请复位后重新读取。';
      this.render();
    }

    accepted(data, path) {
      this.apply(data, true);
      this.nodes.error.textContent = '';
      this.nodes.state.textContent = path.endsWith('/delete') ? '已删除' : '已保存到设备';
    }

    failed() {
      // 超时可能发生在保存之后，先重读列表，不自动重发新增请求。
      this.loaded = false;
      this.nodes.state.textContent = '操作未确认，等待重新读取';
    }

    async load() {
      if (this.loaded || this.loading || !this.canLoad) return;
      const generation = this.generation;
      const token = this.token;
      this.loading = true;
      this.client.refreshControls();
      try {
        const data = await this.client.request('/api/calendar/events', {
          headers: { 'X-Portal-Token': token }
        });
        if (generation !== this.generation || token !== this.token || !this.canLoad) return;
        this.apply(data);
      } catch (error) {
        if (generation !== this.generation || token !== this.token || !this.canRead) return;
        this.nodes.state.textContent = '读取失败';
        this.nodes.error.textContent = error.name === 'AbortError' ? '设备暂时未响应，等待重新读取。' : error.message;
      } finally {
        if (generation === this.generation) this.loading = false;
        this.client.refreshControls();
      }
    }

    render() {
      const entries = [...this.entries].sort((left, right) => {
        if (left.kind !== right.kind) return left.kind === 'important' ? -1 : 1;
        const leftDate = left.kind === 'important' ? left.date.slice(5) : left.date;
        const rightDate = right.kind === 'important' ? right.date.slice(5) : right.date;
        return leftDate.localeCompare(rightDate);
      });
      this.nodes.list.replaceChildren();
      this.nodes.empty.hidden = entries.length !== 0;
      for (const item of entries) {
        const row = document.createElement('tr');
        for (const value of [item.kind === 'important' ? `每年 ${item.date.slice(5)}` : item.date,
          item.kind === 'important' ? '重要日' : '备忘', item.text]) {
          const cell = document.createElement('td');
          cell.textContent = value;
          row.append(cell);
        }
        const actions = document.createElement('td');
        const edit = document.createElement('button');
        edit.type = 'button';
        edit.textContent = '编辑';
        edit.title = '编辑记录';
        edit.addEventListener('click', () => this.edit(item.id));
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.textContent = '删除';
        remove.title = '删除记录';
        remove.addEventListener('click', () => this.remove(item.id));
        actions.append(edit, remove);
        row.append(actions);
        this.nodes.list.append(row);
      }
    }

    edit(id) {
      if (this.blocked) return;
      const item = this.entries.find(value => value.id === id);
      if (!item) return;
      this.editingId = id;
      this.draftRevision = this.revision;
      this.dirty = false;
      this.nodes.kind.value = item.kind;
      this.nodes.date.value = item.date;
      this.nodes.text.value = item.text;
      this.nodes.error.textContent = '';
      this.nodes.state.textContent = '编辑记录';
      this.updateCharacters();
      this.client.refreshControls();
      this.nodes.text.focus();
    }

    submit() {
      if (this.blocked) return;
      this.updateCharacters();
      if (!this.nodes.form.reportValidity()) return;
      this.client.post('/api/calendar/events', new URLSearchParams({
        id: String(this.editingId), revision: String(this.draftRevision),
        kind: this.nodes.kind.value, date: this.nodes.date.value, text: this.nodes.text.value
      }));
    }

    remove(id) {
      if (this.blocked) return;
      const item = this.entries.find(value => value.id === id);
      if (!item) return;
      const revision = this.revision;
      const date = item.kind === 'important' ? `每年 ${item.date.slice(5)}` : item.date;
      if (!window.confirm(`删除 ${date} 的${item.kind === 'important' ? '重要日' : '备忘'}“${item.text}”？`)) return;
      this.client.post('/api/calendar/events/delete', new URLSearchParams({
        id: String(id), revision: String(revision)
      }));
    }
  }

  window.CalendarEvents = CalendarEvents;
})();
