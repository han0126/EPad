(() => {
  'use strict';

  class ImageCropper {
    constructor({ host, stage, canvas, zoom, zoomValue, zoomIn, zoomOut, reset, onChange }) {
      Object.assign(this, { host, canvas, zoom, zoomValue, zoomIn, zoomOut, reset, onChange });
      this.file = null;
      this.cropper = null;
      this.pending = null;
      this.cancelReady = null;
      this.revision = 0;
      this.rotation = 0;
      this.ready = false;
      this.disabled = true;
      this.changing = false;
      this.signature = '';
      zoom.addEventListener('input', () => {
        if (this.ready && !this.disabled) this.cropper.zoomTo(this.zoomBase() * Number(zoom.value));
      });
      zoomIn.addEventListener('click', () => this.zoomBy(1.2));
      zoomOut.addEventListener('click', () => this.zoomBy(1 / 1.2));
      reset.addEventListener('click', () => {
        if (!this.ready || this.disabled) return;
        this.changing = true;
        this.cropper.reset();
        this.cropper.rotateTo(this.rotation);
        this.changing = false;
        this.changed();
      });
      stage.addEventListener('keydown', event => {
        if (!this.ready || this.disabled) return;
        const directions = { ArrowLeft: [-12, 0], ArrowRight: [12, 0], ArrowUp: [0, -12], ArrowDown: [0, 12] };
        if (!directions[event.key]) return;
        event.preventDefault();
        this.cropper.move(...directions[event.key]);
      });
    }

    release() {
      this.revision += 1;
      this.ready = false;
      if (this.cancelReady) this.cancelReady();
      if (this.cropper) this.cropper.destroy();
      this.cropper = null;
      this.file = null;
      this.pending = null;
      this.signature = '';
      this.changing = false;
      this.canvas.width = this.canvas.height = 1;
      this.host.hidden = true;
      this.updateControls();
    }

    setDisabled(disabled) {
      const changed = this.disabled !== disabled;
      this.disabled = disabled;
      if (this.ready) {
        if (disabled) this.cropper.disable();
        else this.cropper.enable();
      }
      this.updateControls();
      if (changed && !disabled && this.ready) this.changed();
    }

    updateControls() {
      for (const control of [this.zoom, this.zoomIn, this.zoomOut, this.reset]) {
        control.disabled = this.disabled || !this.ready;
      }
    }

    zoomBase() {
      const box = this.cropper.getCropBoxData();
      const canvas = this.cropper.getCanvasData();
      const base = Math.max(box.width / canvas.naturalWidth, box.height / canvas.naturalHeight);
      return Number.isFinite(base) && base > 0 ? base : 1;
    }

    updateZoom() {
      if (!this.ready) return;
      const image = this.cropper.getImageData();
      const zoom = image.width / image.naturalWidth / this.zoomBase();
      this.zoom.value = Math.max(1, Math.min(12, zoom)).toFixed(2);
      this.zoomValue.textContent = `${zoom.toFixed(1)}×`;
    }

    zoomBy(factor) {
      if (!this.ready || this.disabled) return;
      const image = this.cropper.getImageData();
      const base = this.zoomBase();
      const ratio = Math.max(base, Math.min(base * 12, image.width / image.naturalWidth * factor));
      this.cropper.zoomTo(ratio);
    }

    selection() {
      if (!this.ready) throw new Error('image_crop_pending');
      const data = this.cropper.getData();
      const canvas = this.cropper.getCanvasData();
      if (![data.x, data.y, data.width, data.height, canvas.naturalWidth, canvas.naturalHeight]
        .every(Number.isFinite) || data.width <= 0 || data.height <= 0 ||
        canvas.naturalWidth <= 0 || canvas.naturalHeight <= 0) throw new Error('image_crop_pending');
      // 用旋转后画布的相对选区定位原图，编辑缩略图不参与最终四色转换。
      const x = Math.max(0, Math.min(1, data.x / canvas.naturalWidth));
      const y = Math.max(0, Math.min(1, data.y / canvas.naturalHeight));
      const width = Math.min(1 - x, data.width / canvas.naturalWidth);
      const height = Math.min(1 - y, data.height / canvas.naturalHeight);
      if (width <= 0 || height <= 0) throw new Error('image_crop_pending');
      return { x, y, width, height };
    }

    changed() {
      if (!this.ready || this.changing || this.disabled) return;
      let signature;
      try {
        this.updateZoom();
        signature = Object.values(this.selection()).map(value => value.toFixed(8)).join(',');
      } catch (error) { signature = 'invalid'; }
      if (signature === this.signature) return;
      this.signature = signature;
      this.onChange();
    }

    setRotation(rotation) {
      if (!this.ready || this.rotation === rotation) return;
      this.rotation = rotation;
      this.changing = true;
      // 切换方向时重新居中，防止旧方向的选区越界；算法切换不改变选区。
      const wasDisabled = this.disabled;
      this.cropper.enable();
      this.cropper.reset();
      this.cropper.rotateTo(rotation);
      this.changing = false;
      this.signature = Object.values(this.selection()).map(value => value.toFixed(8)).join(',');
      this.updateZoom();
      if (wasDisabled) this.cropper.disable();
    }

    async load(file, rotation) {
      rotation = Number(rotation);
      if (this.file === file && this.pending) {
        this.rotation = rotation;
        await this.pending;
        return;
      }
      if (this.file === file && this.ready) {
        this.setRotation(rotation);
        return;
      }
      this.release();
      this.file = file;
      this.rotation = rotation;
      this.host.hidden = false;
      const revision = this.revision;
      const pending = this.build(file, revision);
      this.pending = pending;
      try { await pending; }
      catch (error) {
        if (revision === this.revision) this.release();
        throw error;
      } finally {
        if (this.pending === pending) this.pending = null;
      }
    }

    async build(file, revision) {
      const style = document.getElementById('cropper-style');
      if (!window.Cropper || !window.ImageTools || !style || !style.sheet) throw new Error('image_crop_tools');
      const source = await window.ImageTools.editorSource(file, {
        isCurrent: () => revision === this.revision
      });
      if (revision !== this.revision) {
        source.width = source.height = 1;
        throw new Error('image_cancelled');
      }
      this.canvas.width = source.width;
      this.canvas.height = source.height;
      const context = this.canvas.getContext('2d');
      if (!context) throw new Error('image_decode');
      context.drawImage(source, 0, 0);
      source.width = source.height = 1;
      await new Promise((resolve, reject) => {
        let timer = null;
        let settled = false;
        const finish = error => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          if (this.cancelReady === cancelReady) this.cancelReady = null;
          if (error) reject(error);
          else resolve();
        };
        const cancelReady = () => finish(new Error('image_cancelled'));
        this.cancelReady = cancelReady;
        timer = setTimeout(() => finish(new Error('image_crop_tools')), 15000);
        try {
          this.cropper = new window.Cropper(this.canvas, {
            aspectRatio: 4 / 3,
            viewMode: 1,
            dragMode: 'move',
            autoCropArea: 1,
            cropBoxMovable: false,
            cropBoxResizable: false,
            toggleDragModeOnDblclick: false,
            scalable: false,
            checkOrientation: false,
            checkCrossOrigin: false,
            minContainerWidth: 120,
            minContainerHeight: 90,
            ready: () => {
              // 延后一轮微任务，确保构造完成，缓存命中时也不会访问未赋值的实例。
              Promise.resolve().then(() => {
                if (revision !== this.revision || settled) return;
                try {
                  this.changing = true;
                  this.cropper.rotateTo(this.rotation);
                  this.ready = true;
                  this.changing = false;
                  this.signature = Object.values(this.selection()).map(value => value.toFixed(8)).join(',');
                  this.updateZoom();
                  this.setDisabled(this.disabled);
                  finish();
                } catch (error) { finish(new Error('image_crop_tools')); }
              });
            },
            crop: () => {
              if (revision === this.revision) this.changed();
            },
            zoom: event => {
              if (revision !== this.revision) { event.preventDefault(); return; }
              if (!this.ready) return;
              if (!Number.isFinite(event.detail.ratio) || event.detail.ratio > this.zoomBase() * 12) {
                event.preventDefault();
              }
            }
          });
        } catch (error) { finish(new Error('image_crop_tools')); }
      });
    }
  }

  window.ImageCropper = ImageCropper;
})();
