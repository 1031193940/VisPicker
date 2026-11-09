// Node.js 22+. Use the automatic-saved.gif from a short VisPickRecordingIntegration run.
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { access, mkdtemp, readFile, realpath, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';

const patternRow = 80;
const patternWidth = 307;
const probePeriods = 3;
const palette = ['193c5b', '6d486d', '63a375', 'ffd166', 'dfdfdf'];
const expectedRows = Array.from({ length: 8 }, (_, marker) =>
    Array.from({ length: patternWidth }, (_, x) => x >= 25 + marker * 35 && x < 55 + marker * 35
        ? 'ed2939' : palette[(Math.floor(x / 37) + Math.floor(patternRow / 29)) % palette.length]).join(''));
const browserPaths = [
    ['Chrome', 'Google/Chrome/Application/chrome.exe'],
    ['Edge', 'Microsoft/Edge/Application/msedge.exe'],
    ['Firefox', 'Mozilla Firefox/firefox.exe'],
];

// Read only GIF block lengths and delays. Pixels are decoded and composited by the browsers.
function readFixture(gif) {
    if (gif.length < 14 || gif.toString('ascii', 0, 6) !== 'GIF89a') throw Error('Expected a GIF89a fixture');
    const width = gif.readUInt16LE(6);
    const height = gif.readUInt16LE(8);
    let offset = 13 + ((gif[10] & 128) ? 3 * (2 << (gif[10] & 7)) : 0);
    let delayMs = 0;
    let durationMs = 0;
    let frameCount = 0;
    let loops = false;
    while (offset < gif.length && gif[offset] !== 0x3b) {
        const block = gif[offset++];
        if (block === 0x21) {
            const label = gif[offset++];
            if (label === 0xf9) delayMs = 10 * gif.readUInt16LE(offset + 2);
            if (label === 0xff && gif.toString('ascii', offset + 1, offset + 12) === 'NETSCAPE2.0') {
                loops = gif.readUInt16LE(offset + 14) === 0;
            }
        } else if (block === 0x2c) {
            const flags = gif[offset + 8];
            offset += 10 + ((flags & 128) ? 3 * (2 << (flags & 7)) : 0);
            durationMs += delayMs;
            delayMs = 0;
            ++frameCount;
        } else {
            throw Error('Unexpected GIF fixture block');
        }
        while (offset < gif.length && gif[offset] !== 0) offset += 1 + gif[offset];
        ++offset;
    }
    if (gif[offset] !== 0x3b || !loops || durationMs < 1500 || durationMs > 5000 ||
        width < patternWidth || height <= patternRow || frameCount < 8) {
        throw Error('Use automatic-saved.gif from the default 2-second recording integration run');
    }
    return { width, height, frame_count: frameCount, duration_ms: durationMs };
}

async function openProtocol(url) {
    const socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => { socket.close(); reject(Error('Browser connection timed out')); }, 5000);
        socket.addEventListener('open', () => { clearTimeout(timeout); resolve(); }, { once: true });
        socket.addEventListener('error', () => { clearTimeout(timeout); reject(Error('Browser connection failed')); }, { once: true });
    });
    let nextId = 0;
    return {
        close: () => socket.close(),
        command(method, params = {}) {
            return new Promise((resolve, reject) => {
                if (socket.readyState !== WebSocket.OPEN) return reject(Error('Browser connection closed'));
                const id = ++nextId;
                function finish() {
                    clearTimeout(timeout);
                    socket.removeEventListener('message', onMessage);
                    socket.removeEventListener('close', onClose);
                }
                function onMessage(event) {
                    const reply = JSON.parse(event.data);
                    if (reply.id !== id) return;
                    finish();
                    if (reply.error) reject(Error(`${method}: ${reply.message ?? reply.error.message ?? reply.error}`));
                    else resolve(reply.result);
                }
                function onClose() { finish(); reject(Error(`${method}: Browser connection closed`)); }
                const timeout = setTimeout(() => { finish(); reject(Error(`${method}: Browser command timed out`)); }, 5000);
                socket.addEventListener('message', onMessage);
                socket.addEventListener('close', onClose);
                socket.send(JSON.stringify({ id, method, params }));
            });
        },
    };
}

async function waitForEndpoint(child, profile, firefox) {
    let stderr = '';
    let launchError;
    child.once('error', error => { launchError = error; });
    child.stderr.on('data', data => { stderr = (stderr + data).slice(-4096); });
    child.stdout.resume();
    for (let attempt = 0; attempt < 100; ++attempt) {
        if (launchError) throw launchError;
        if (firefox) {
            const address = stderr.match(/WebDriver BiDi listening on (ws:\/\/127\.0\.0\.1:\d+)/)?.[1];
            if (address) return `${address}/session`;
        } else {
            // Edge's launcher can exit before the browser process writes this file.
            const activePort = await readFile(join(profile, 'DevToolsActivePort'), 'utf8').catch(() => '');
            const [port, endpoint] = activePort.trim().split(/\r?\n/);
            if (/^\d+$/.test(port) && endpoint?.startsWith('/devtools/browser/')) {
                return `ws://127.0.0.1:${port}${endpoint}`;
            }
        }
        await delay(100);
    }
    throw Error('No local browser debugging endpoint');
}

async function attachFirefox(protocol, pageUrl) {
    const { command } = protocol;
    const { capabilities } = await command('session.new', { capabilities: {} });
    const { contexts: [{ context }] } = await command('browsingContext.getTree');
    await command('browsingContext.setViewport', { context, viewport: { width: 900, height: 540 }, devicePixelRatio: 1 });
    await command('browsingContext.navigate', { context, url: pageUrl, wait: 'complete' });
    return {
        version: capabilities.browserVersion,
        async evaluate(expression) {
            const result = await command('script.evaluate', { expression, target: { context }, awaitPromise: true });
            if (result.type !== 'success') throw Error(result.exceptionDetails.text);
            return result.result.value;
        },
        capture: () => command('browsingContext.captureScreenshot', {
            context, clip: { type: 'box', x: 0, y: patternRow, width: patternWidth, height: 1 },
        }),
    };
}

async function attachChromium(browser, pageUrl) {
    const { product } = await browser.command('Browser.getVersion');
    const { targetInfos } = await browser.command('Target.getTargets');
    const target = targetInfos.find(info => info.type === 'page');
    const page = await openProtocol(new URL(`/devtools/page/${target.targetId}`, browser.url));
    try {
        const { command } = page;
        await command('Emulation.setDeviceMetricsOverride', { width: 900, height: 540, deviceScaleFactor: 1, mobile: false });
        const { errorText } = await command('Page.navigate', { url: pageUrl });
        if (errorText) throw Error(errorText);
        return {
            version: product,
            close: page.close,
            async evaluate(expression) {
                const result = await command('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true });
                if (result.exceptionDetails) throw Error(result.exceptionDetails.text);
                return result.result.value;
            },
            capture: () => command('Page.captureScreenshot', {
                format: 'png', clip: { x: 0, y: patternRow, width: patternWidth, height: 1, scale: 1 },
            }),
        };
    } catch (error) {
        page.close();
        throw error;
    }
}

async function checkPlayback(page, fixture, result) {
    const imageReady = `(async () => {
        const image = document.querySelector('img');
        if (!image) return false;
        await image.decode();
        return image.naturalWidth === ${fixture.width} && image.naturalHeight === ${fixture.height};
    })()`;
    let ready = false;
    for (let attempt = 0; attempt < 50 && !ready; ++attempt) {
        ready = await page.evaluate(imageReady);
        if (!ready) await delay(20);
    }
    if (!ready) throw Error('Browser did not decode the GIF at its expected dimensions');
    const start = performance.now();
    result.samples = [];
    for (let period = 0; period < probePeriods; ++period) {
        const samples = [];
        result.samples.push(samples);
        while (performance.now() - start < (period + 1) * fixture.duration_ms) {
            const sample = { elapsed_ms: Math.round(performance.now() - start) };
            const { data } = await page.capture();
            // Decode the static screenshot PNG here. Drawing the animated GIF to a canvas would read its first frame.
            const rowHex = await page.evaluate(`(async () => {
                const screenshot = new Image();
                screenshot.src = 'data:image/png;base64,${data}';
                await screenshot.decode();
                if (screenshot.width !== ${patternWidth} || screenshot.height !== 1) throw Error('Unexpected screenshot size');
                const canvas = document.createElement('canvas');
                canvas.width = screenshot.width;
                canvas.height = 1;
                const context = canvas.getContext('2d');
                context.drawImage(screenshot, 0, 0);
                return Array.from(context.getImageData(0, 0, canvas.width, 1).data)
                    .filter((_, index) => index % 4 !== 3).map(value => value.toString(16).padStart(2, '0')).join('');
            })()`);
            sample.marker = expectedRows.indexOf(rowHex);
            if (sample.marker === -1) throw Error(`Unexpected GIF row at ${sample.elapsed_ms} ms`);
            samples.push(sample);
            await delay(20);
        }
        if (new Set(samples.map(sample => sample.marker)).size < 4) {
            throw Error(`Too few frame changes in GIF-duration period ${period + 1}`);
        }
    }
    if (new Set(result.samples.flat().map(sample => sample.marker)).size !== 8) {
        throw Error('Some fixture marker positions were never rendered');
    }
}

async function runBrowser(name, executable, fixture, gif, profile) {
    const firefox = name === 'Firefox';
    const result = { browser: name, status: 'failed' };
    let child;
    let browser;
    let page;
    try {
        const pagePath = join(profile, 'fixture.html');
        await writeFile(pagePath, `<!doctype html><meta charset="utf-8"><style>body{margin:0}img{display:block}</style>
            <img src="data:image/gif;base64,${gif.toString('base64')}">`);
        const args = firefox
            ? ['-headless', '-no-remote', '-wait-for-browser', '-offline', '-profile', profile, '--remote-debugging-port', '0']
            : ['--headless', '--no-first-run', '--disable-background-networking', '--disable-extensions',
                '--disable-sync', `--user-data-dir=${profile}`, '--remote-debugging-port=0'];
        child = spawn(executable, [...args, 'about:blank'], { windowsHide: true });
        const url = await waitForEndpoint(child, profile, firefox);
        browser = { ...await openProtocol(url), url };
        const pageUrl = pathToFileURL(pagePath).href;
        page = await (firefox ? attachFirefox(browser, pageUrl) : attachChromium(browser, pageUrl));
        result.version = page.version;
        await checkPlayback(page, fixture, result);
        result.status = 'passed';
    } catch (error) {
        result.error = error.message;
    } finally {
        page?.close?.();
        if (browser) {
            await browser.command(firefox ? 'browser.close' : 'Browser.close').catch(() => {});
            browser.close();
        } else {
            child?.kill();
        }
    }
    return result;
}

async function findBrowser(relativePath) {
    for (const directory of [process.env.ProgramFiles, process.env['ProgramFiles(x86)'], process.env.LOCALAPPDATA]) {
        if (!directory) continue;
        const executable = join(directory, relativePath);
        if (await access(executable).then(() => true, () => false)) return executable;
    }
}

const [, , fixturePath, outputPath] = process.argv;
if (!fixturePath || !outputPath) throw Error('Usage: node tests/e2e/CheckBrowserGif.mjs automatic-saved.gif browser-gif.json');
const gif = await readFile(fixturePath);
const fixture = readFixture(gif);
const report = {
    started_utc: new Date().toISOString(),
    fixture: { ...fixture, name: basename(fixturePath), sha256: createHash('sha256').update(gif).digest('hex') },
    method: 'Installed browsers, isolated headless profiles, rendered GIF row samples over three source-duration periods',
    browsers: [],
};
const temporaryRoot = await realpath(tmpdir());
for (const [name, relativePath] of browserPaths) {
    const executable = await findBrowser(relativePath);
    if (!executable) {
        report.browsers.push({ browser: name, status: 'not-tested', reason: 'Browser not found in standard install directories' });
        continue;
    }
    const profile = await mkdtemp(join(temporaryRoot, 'VisPick-browser-gif-'));
    const result = await runBrowser(name, executable, fixture, gif, profile);
    try {
        if (dirname(await realpath(profile)) !== temporaryRoot) throw Error('Unexpected profile directory');
        await rm(profile, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
    } catch (error) {
        result.profile_cleanup_warning = error.message;
    }
    report.browsers.push(result);
    console.log(`${name}: ${result.status}${result.error ? ` (${result.error})` : ''}`);
}
report.finished_utc = new Date().toISOString();
await writeFile(outputPath, JSON.stringify(report, null, 2) + '\n', { flag: 'wx' });
process.exitCode = report.browsers.every(result => result.status === 'passed' && !result.profile_cleanup_warning) ? 0 : 1;
