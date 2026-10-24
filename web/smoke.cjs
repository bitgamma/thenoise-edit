// SPDX-License-Identifier: Apache-2.0
// Headless smoke test for the real WASM UI. Run after building: node web/smoke.cjs
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const repo = path.resolve(__dirname, '..');
const output = path.join(repo, 'out', 'web-smoke');
const webBuild = path.join(repo, 'build-web');
const browserTools = path.join(os.homedir(), '.local', 'share', 'thenoise-web-tools');
const { chromium } = require(require.resolve('playwright', { paths: [browserTools] }));
const port = Number(process.env.NOISE_WEB_TEST_PORT || 8765);
const url = `http://127.0.0.1:${port}/noise-edit.html`;
const viewport = { width: 1280, height: 800 };
const canvasCheckRegion = { x: 390, y: 330, width: 120, height: 120 };
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function serverReady() {
  for (let attempt = 0; attempt < 50; attempt++) {
    try {
      const response = await fetch(url);
      if (response.ok) return;
    } catch (_) { /* The server may not have bound its port yet. */ }
    await sleep(200);
  }
  throw new Error(`Could not reach ${url}`);
}

async function editorReady(page) {
  await page.goto(url);
  await page.waitForFunction(() => {
    const canvas = document.querySelector('#canvas');
    return canvas && canvas.width > 0 && canvas.height > 0 &&
      document.querySelector('#status')?.textContent === '' &&
      typeof Module?.ccall === 'function';
  }, null, { timeout: 60000 });
}

async function makePaintedDocument(page) {
  await page.locator('button[data-action="4"]').click();
  await page.waitForTimeout(300);
  await page.mouse.click(292, 557);
  await page.waitForTimeout(300);
  await page.mouse.move(1100, 700);
  await page.mouse.click(450, 390);
  await page.mouse.move(1100, 700);
  await page.waitForTimeout(150);
}

async function openWithPicker(page, filePath) {
  const picker = page.waitForEvent('filechooser');
  await page.locator('button[data-action="0"]').click();
  await (await picker).setFiles(filePath);
  await page.waitForTimeout(500);
}

async function saveDownload(page, action, name, directory, errors) {
  const prompt = page.waitForEvent('dialog', { timeout: 5000 })
    .then((dialog) => dialog.accept(name));
  const completed = page.waitForEvent('download', { timeout: 10000 }).catch((error) => error);
  await Promise.all([
    page.locator(`button[data-action="${action}"]`).click(),
    prompt
  ]);
  const download = await completed;
  if (download instanceof Error) {
    const files = await page.evaluate(() => {
      try { return FS.readdir('/downloads'); } catch (failure) { return String(failure); }
    });
    throw new Error(`${download.message}; /downloads=${JSON.stringify(files)}; browser errors=${errors.join(' | ')}`);
  }
  const filePath = path.join(directory, download.suggestedFilename());
  await download.saveAs(filePath);
  return filePath;
}

function trackFileFlowErrors(page, errors) {
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => {
    const text = message.text();
    if (message.type() === 'error' && !/^\[(DEBUG|INFO|WARN)\]/.test(text)) errors.push(text);
  });
}

async function checkTheme(browser, theme) {
  const context = await browser.newContext({ viewport });
  try {
    await context.addInitScript((selected) => {
      localStorage.setItem('thenoise.config', JSON.stringify({ ui_theme: selected }));
    }, theme);
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', (error) => errors.push(error.message));
    page.on('response', (response) => {
      if (response.status() >= 400) errors.push(`${response.status()} ${response.url()}`);
    });
    await editorReady(page);
    const webgl2 = await page.evaluate(() => Boolean(document.createElement('canvas').getContext('webgl2')));
    assert(webgl2, 'Chromium cannot create a WebGL2 context');
    assert.equal(await page.locator('html').getAttribute('data-theme'), theme);
    await page.locator('button[data-action="4"]').click();  // Real browser-to-WASM action.
    await page.waitForTimeout(300);
    await page.mouse.click(292, 557);  // Create canvas in the New dialog at the test viewport.
    await page.waitForTimeout(300);
    await page.mouse.move(1100, 700);
    const editRegion = canvasCheckRegion;
    const beforeStroke = await page.screenshot({ clip: editRegion });
    await page.mouse.click(450, 390);
    await page.mouse.move(1100, 700);
    await page.waitForTimeout(150);
    const afterStroke = await page.screenshot({ clip: editRegion });
    assert.notDeepEqual(afterStroke, beforeStroke, `${theme}: brush stroke changes canvas pixels`);
    await page.keyboard.press('Control+z');
    await page.waitForTimeout(150);
    const afterUndo = await page.screenshot({ clip: editRegion });
    assert.deepEqual(afterUndo, beforeStroke, `${theme}: undo restores canvas pixels`);
    assert.deepEqual(errors, [], `${theme}: browser errors`);
    await page.screenshot({ path: path.join(output, `${theme}.png`) });
    console.log(`${theme}: WebGL2, WASM startup, create, brush, undo, screenshot OK`);
  } finally {
    await context.close();
  }
}

async function checkFileFlow(browser) {
  const context = await browser.newContext({ viewport, acceptDownloads: true });
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'noise-web-smoke-'));
  const errors = [];
  try {
    const page = await context.newPage();
    trackFileFlowErrors(page, errors);
    await editorReady(page);
    await makePaintedDocument(page);
    const savedCanvas = await page.screenshot({ clip: canvasCheckRegion });
    const project = await saveDownload(page, 2, 'noise-smoke.noise', temporary, errors);
    assert(fs.statSync(project).size > 0, 'Save project produced an empty download');

    await page.close();
    const reopenedProject = await context.newPage();
    trackFileFlowErrors(reopenedProject, errors);
    await editorReady(reopenedProject);
    await openWithPicker(reopenedProject, project);
    assert.deepEqual(await reopenedProject.screenshot({ clip: canvasCheckRegion }), savedCanvas,
                     'opening the saved project restores the visible canvas');

    const image = await saveDownload(reopenedProject, 3, 'noise-smoke.png', temporary, errors);
    const pngHeader = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
    assert(fs.readFileSync(image).subarray(0, pngHeader.length).equals(pngHeader),
           'Export image produced a PNG');
    await reopenedProject.close();

    const reopenedImage = await context.newPage();
    trackFileFlowErrors(reopenedImage, errors);
    await editorReady(reopenedImage);
    await openWithPicker(reopenedImage, image);
    assert.deepEqual(await reopenedImage.screenshot({ clip: canvasCheckRegion }), savedCanvas,
                     'opening the exported PNG preserves the visible canvas');
    assert.deepEqual(errors, [], 'browser file flow has no page errors');
    console.log('WASM file flow: save/open project, export/open PNG OK');
  } finally {
    await context.close();
    fs.rmSync(temporary, { recursive: true, force: true });
  }
}

(async () => {
  assert(fs.existsSync(path.join(webBuild, 'noise-edit.html')), 'Build web target first');
  fs.mkdirSync(output, { recursive: true });
  const server = spawn('python3', ['-m', 'http.server', String(port), '--bind', '127.0.0.1', '--directory', webBuild], { stdio: 'ignore' });
  let browser;
  try {
    await serverReady();
    browser = await chromium.launch({ headless: true, args: [
      '--enable-webgl', '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'
    ] });
    for (const theme of ['dark', 'light']) await checkTheme(browser, theme);
    await checkFileFlow(browser);
  } finally {
    if (browser) await browser.close();
    server.kill('SIGTERM');
  }
})().catch((error) => { console.error(error); process.exitCode = 1; });
