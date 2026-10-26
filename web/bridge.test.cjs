// SPDX-License-Identifier: Apache-2.0
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const paths = new Map();
const directories = new Set(['/uploads', '/downloads']);
const callbacks = [];
const elements = [];
const listeners = {};
const ctx = {
  FS: {
    mkdir: (path) => directories.add(path),
    writeFile: (path, bytes) => paths.set(path, bytes),
    readFile: (path) => {
      assert(paths.has(path), `missing ${path}`);
      return paths.get(path);
    }
  },
  Module: {
    canvas: {addEventListener: (name, fn) => { listeners[name] = fn; }},
    ccall: (name, _type, _argTypes, args) => callbacks.push({name, args})
  },
  document: {
    body: {
      appendChild: (element) => elements.push(element)
    },
    createElement: (kind) => ({
      kind, style: {}, click() { this.clicked = true; }, remove() {}
    })
  },
  window: {
    prompt: (_title, proposed) => proposed,
    setTimeout: () => {}
  },
  URL: {createObjectURL: () => 'blob:mock', revokeObjectURL: () => {}},
  Blob: class Blob {constructor(chunks, options) { this.chunks = chunks; this.options = options; }},
  console
};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync('web/bridge.js', 'utf8'), ctx);

(async () => {
  ctx.Module.noisePick('save', 'Save project', '', '.noise', false);
  assert.deepEqual(JSON.parse(callbacks.pop().args[0]), ['/downloads/document.noise']);

  ctx.window.prompt = () => null;
  ctx.Module.noisePick('save', 'Save project', '', '.noise', false);
  assert.equal(callbacks.pop().args[2], 1);

  ctx.Module.noisePick('open', 'Open image', '', '.png,.jpg', false);
  const input = elements.at(-1);
  input.files = [{name: 'example.png', arrayBuffer: async () => new Uint8Array([137, 80, 78, 71]).buffer}];
  await input.onchange();
  const result = callbacks.pop();
  assert.equal(result.name, 'noise_web_pick_callback');
  const [path] = JSON.parse(result.args[0]);
  assert.match(path, /^\/uploads\/1\/example\.png$/);
  assert.deepEqual([...paths.get(path)], [137, 80, 78, 71]);

  assert.equal(ctx.Module.noiseDownload(path), true);
  const anchor = elements.at(-1);
  assert.equal(anchor.download, 'example.png');
  assert.equal(anchor.clicked, true);

  ctx.Module.noiseEnableDrop();
  assert.equal(typeof listeners.drop, 'function');
  await listeners.drop({preventDefault() {}, dataTransfer: {
    files: [{name: '../wrong.jpg', arrayBuffer: async () => new Uint8Array([255]).buffer}]
  }});
  const dropped = callbacks.pop();
  assert.equal(dropped.name, 'noise_web_dropped');
  assert.match(dropped.args[0], /^\/uploads\/2\/\.\._wrong\.jpg$/);
  assert.equal(paths.get(dropped.args[0])[0], 255);
  console.log('Browser bridge: save, cancel, import, download, drop OK');
})().catch((error) => { console.error(error); process.exitCode = 1; });
