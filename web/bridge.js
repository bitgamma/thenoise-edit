// SPDX-License-Identifier: Apache-2.0
var Module = typeof Module !== 'undefined' ? Module : {};

(function () {
  let nextFile = 0;
  const copyFile = async (file) => {
    const directory = `/uploads/${++nextFile}`;
    FS.mkdir(directory);
    const clean = file.name.replace(/[\\/\u0000-\u001f]/g, '_') || 'image';
    const path = `${directory}/${clean}`;
    FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
    return path;
  };

  const picked = (paths, error = '', cancelled = false) => {
    Module.ccall('noise_web_pick_callback', null,
      ['string', 'string', 'number'], [JSON.stringify(paths), error, cancelled ? 1 : 0]);
  };

  Module['noisePick'] = (kind, title, initial, accept, multiple) => {
    if (kind === 'save') {
      const suffix = accept.split(',')[0] || '.noise';
      const proposed = initial.split('/').pop() || (suffix === '.noise' ? 'document.noise' : `image${suffix}`);
      const name = window.prompt(title, proposed);
      if (name === null) return picked([], '', true);
      const clean = name.split(/[\\/]/).pop().trim();
      if (!clean) return picked([], '', true);
      const ext = clean.includes('.') ? '' : suffix;
      return picked([`/downloads/${clean}${ext}`]);
    }

    const input = document.createElement('input');
    input.type = 'file';
    input.accept = accept;
    input.multiple = multiple;
    input.style.display = 'none';
    document.body.appendChild(input);
    let completed = false;
    const finish = (paths, error = '', cancelled = false) => {
      if (completed) return;
      completed = true;
      input.remove();
      picked(paths, error, cancelled);
    };
    input.onchange = async () => {
      try {
        const paths = [];
        for (const file of input.files) paths.push(await copyFile(file));
        finish(paths, '', paths.length === 0);
      } catch (error) { finish([], String(error)); }
    };
    input.oncancel = () => finish([], '', true);
    input.click();
  };

  Module['noiseDownload'] = (path) => {
    try {
      const bytes = FS.readFile(path);
      const extension = path.split('.').pop().toLowerCase();
      const mime = ({png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg',
                     webp: 'image/webp', noise: 'application/octet-stream'})[extension]
                     || 'application/octet-stream';
      const blob = new Blob([bytes], {type: mime});
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = path.split('/').pop();
      document.body.appendChild(anchor);
      anchor.click();
      anchor.remove();
      window.setTimeout(() => URL.revokeObjectURL(url), 30000);
      return true;
    } catch (error) {
      console.error('TheNoise download failed:', error);
      return false;
    }
  };

  Module['noiseEnableDrop'] = () => {
    const target = Module.canvas;
    target.addEventListener('dragover', (event) => event.preventDefault());
    target.addEventListener('drop', async (event) => {
      event.preventDefault();
      try {
        for (const file of event.dataTransfer.files) {
          const path = await copyFile(file);
          Module.ccall('noise_web_dropped', null, ['string'], [path]);
        }
      } catch (error) { console.error('TheNoise file drop:', error); }
    });
  };
})();
