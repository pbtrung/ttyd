// Real-browser regression checks. Build ttyd first; set CHROMIUM_PATH if needed.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { chromium } = require('playwright-core');

const binary = process.env.TTYD_BINARY || path.resolve(__dirname, '../../build/ttyd');
const executablePath = process.env.CHROMIUM_PATH || '/usr/bin/chromium';
const profiling = process.argv.includes('--profile');

async function main() {
    assert(fs.existsSync(binary), `Build ttyd first: ${binary}`);
    const server = spawn(binary, ['-i', '127.0.0.1', '-p', '0', '-W', 'bash', '--noprofile', '--norc']);
    let browser;
    try {
        const port = await new Promise((resolve, reject) => {
            let log = '';
            const timeout = setTimeout(() => reject(new Error(`Server startup timed out: ${log}`)), 10000);
            server.once('error', reject);
            server.once('exit', code => reject(new Error(`Server exited (${code}): ${log}`)));
            server.stderr.on('data', chunk => {
                log += chunk;
                const match = /Listening on port: (\d+)/.exec(log);
                if (match) {
                    clearTimeout(timeout);
                    resolve(Number(match[1]));
                }
            });
        });
        browser = await chromium.launch({ executablePath, headless: true, args: ['--no-sandbox'] });
        for (const deviceScaleFactor of (process.env.TTYD_TEST_SCALES || '1,1.25,2').split(',').map(Number)) {
            const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, deviceScaleFactor });
            try {
                const page = await context.newPage();
                page.setDefaultTimeout(15000);
                const errors = [];
                page.on('pageerror', error => errors.push(error.message));
                page.on('dialog', dialog => dialog.accept());
                await page.goto(`http://127.0.0.1:${port}/?fontWidth=0.9`);
                await page.waitForFunction(
                    () => window.term?.buffer.active.getLine(0)?.translateToString(true).length > 0
                );
                await page.evaluate(() => {
                    window.stats = { resizes: 0, lines: 0, glyphs: 0, viewportDecodes: 0, maxDecodesPerPaint: 0 };
                    let painting = false;
                    let decodes = 0;
                    const renderer = window.term.renderer;
                    const render = renderer.render.bind(renderer);
                    renderer.render = (...args) => {
                        decodes = 0;
                        painting = true;
                        try {
                            return render(...args);
                        } finally {
                            painting = false;
                            window.stats.maxDecodesPerPaint = Math.max(window.stats.maxDecodesPerPaint, decodes);
                        }
                    };
                    const viewport = window.term.wasmTerm.getViewport.bind(window.term.wasmTerm);
                    window.term.wasmTerm.getViewport = (...args) => {
                        if (painting) {
                            decodes++;
                            window.stats.viewportDecodes++;
                        }
                        return viewport(...args);
                    };
                    for (const [method, counter] of [
                        ['resize', 'resizes'],
                        ['renderLine', 'lines'],
                    ]) {
                        const original = renderer[method].bind(renderer);
                        renderer[method] = (...args) => {
                            window.stats[counter]++;
                            return original(...args);
                        };
                    }
                    const fillText = CanvasRenderingContext2D.prototype.fillText;
                    CanvasRenderingContext2D.prototype.fillText = function (...args) {
                        window.stats.glyphs++;
                        return fillText.apply(this, args);
                    };
                });
                const client = await context.newCDPSession(page);
                await client.send('Performance.enable');
                const report = { deviceScaleFactor, cases: {} };
                const metrics = async () =>
                    Object.fromEntries(
                        (await client.send('Performance.getMetrics')).metrics.map(m => [m.name, m.value])
                    );
                async function measure(name, action, settle = 350) {
                    await page.waitForTimeout(settle);
                    await page.evaluate(() => {
                        for (const key of Object.keys(window.stats)) window.stats[key] = 0;
                    });
                    const before = await metrics();
                    const start = Date.now();
                    if (action) await action();
                    else await page.waitForTimeout(1600);
                    const after = await metrics();
                    const counts = await page.evaluate(() => window.stats);
                    assert(counts.maxDecodesPerPaint <= 1, `${name}: repeated viewport decoding`);
                    const result = {
                        wallMs: Date.now() - start,
                        taskMs: Math.round((after.TaskDuration - before.TaskDuration) * 1000),
                        ...counts,
                    };
                    report.cases[name] = result;
                    console.log(JSON.stringify({ scale: deviceScaleFactor, case: name, ...result }));
                    return result;
                }
                async function quiet(name, settle = 350) {
                    const result = await measure(name, null, settle);
                    assert.equal(result.resizes, 0, `${name}: spontaneous canvas resize`);
                    assert.equal(result.lines, 0, `${name}: unchanged rows repainted`);
                    assert.equal(result.glyphs, 0, `${name}: unchanged glyphs repainted`);
                }
                async function pixels(name) {
                    const consistent = await page.evaluate(() => {
                        const term = window.term;
                        const canvas = term.element.querySelector('canvas');
                        const partial = canvas.toDataURL();
                        const oldPixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
                        term.renderer.render(term.wasmTerm, true, term.getViewportY(), term, term.scrollbarOpacity);
                        const full = canvas.toDataURL();
                        if (partial === full) return { equal: true };
                        const newPixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
                        let count = 0,
                            x1 = canvas.width,
                            x2 = 0,
                            y1 = canvas.height,
                            y2 = 0;
                        for (let i = 0; i < oldPixels.length; i += 4) {
                            if (
                                oldPixels[i] !== newPixels[i] ||
                                oldPixels[i + 1] !== newPixels[i + 1] ||
                                oldPixels[i + 2] !== newPixels[i + 2] ||
                                oldPixels[i + 3] !== newPixels[i + 3]
                            ) {
                                count++;
                                const x = (i / 4) % canvas.width,
                                    y = Math.floor(i / 4 / canvas.width);
                                x1 = Math.min(x1, x);
                                x2 = Math.max(x2, x);
                                y1 = Math.min(y1, y);
                                y2 = Math.max(y2, y);
                            }
                        }
                        return { equal: false, partial, full, count, bounds: { x1, x2, y1, y2 } };
                    });
                    if (!consistent.equal) {
                        const directory = process.env.TTYD_TEST_ARTIFACTS || '/tmp/ttyd-renderer-tests';
                        fs.mkdirSync(directory, { recursive: true });
                        for (const which of ['partial', 'full'])
                            fs.writeFileSync(
                                path.join(directory, `${deviceScaleFactor}-${name.replaceAll(' ', '_')}-${which}.png`),
                                Buffer.from(consistent[which].split(',')[1], 'base64')
                            );
                        throw new Error(
                            `${name}: incremental paint differs from a full repaint (${
                                consistent.count
                            } pixels, ${JSON.stringify(consistent.bounds)}); artifacts in ${directory}`
                        );
                    }
                }
                async function output(expected) {
                    try {
                        await page.waitForFunction(
                            text =>
                                Array.from({ length: window.term.rows }, (_, row) =>
                                    window.term.buffer.active
                                        .getLine(window.term.buffer.active.length - window.term.rows + row)
                                        ?.translateToString(true)
                                ).some(line => line?.includes(text)),
                            expected
                        );
                    } catch (error) {
                        const screen = await page.evaluate(() => ({
                            focus: document.activeElement?.tagName,
                            viewport: window.term.getViewportY(),
                            lines: Array.from({ length: window.term.rows }, (_, row) =>
                                window.term.buffer.active
                                    .getLine(window.term.buffer.active.length - window.term.rows + row)
                                    ?.translateToString(true)
                            ),
                        }));
                        throw new Error(`Missing output ${expected}: ${JSON.stringify(screen)}`, { cause: error });
                    }
                    await page.waitForTimeout(100);
                }
                async function command(text) {
                    await page.keyboard.type(text);
                    await page.keyboard.press('Enter');
                }

                await quiet('idle');
                if (profiling) {
                    await client.send('Profiler.enable');
                    await client.send('Profiler.start');
                }
                const typing = await measure('typing', () =>
                    page.keyboard.type('abcdefghijklmnopqrstuvwxyz'.repeat(3), { delay: 20 })
                );
                assert.equal(typing.resizes, 0);
                assert(typing.lines < 78 * 8, 'typing repainted far more than the changed rows');
                if (profiling) {
                    const { profile } = await client.send('Profiler.stop');
                    report.hotspots = profile.nodes
                        .filter(node => node.hitCount)
                        .sort((a, b) => b.hitCount - a.hitCount)
                        .slice(0, 8)
                        .map(node => ({ function: node.callFrame.functionName, samples: node.hitCount }));
                }
                await quiet('afterTyping');
                await pixels('typing');
                await page.keyboard.press('Control+u');
                await command("printf '\\nANSI_\\033[31mRED\\033[0m_世界_✓\\n'");
                await output('ANSI_RED_世界_✓');
                await pixels('ANSI and Unicode');

                await page.evaluate(() => {
                    const data = new DataTransfer();
                    data.setData('text/plain', "printf '\\nPASTE_OK\\n'");
                    window.term.textarea.dispatchEvent(
                        new ClipboardEvent('paste', { clipboardData: data, bubbles: true, cancelable: true })
                    );
                });
                await page.keyboard.press('Enter');
                await output('PASTE_OK');
                await pixels('paste');

                await page.evaluate(() => window.term.select(0, 0, 8));
                await quiet('selection');
                await pixels('selection');
                await page.evaluate(() => window.term.clearSelection());
                await quiet('selectionCleared');
                await pixels('selection cleared');

                await page.evaluate(() => (window.term.options.cursorBlink = true));
                const blink = await measure('cursorBlink');
                assert(blink.lines > 0 && blink.lines < 12, 'cursor blink redraws at the wrong frequency');
                await pixels('cursor blinking');
                await page.evaluate(() => (window.term.options.cursorBlink = false));
                await quiet('blinkDisabled');

                await command('for n in $(seq 1 160); do printf \'SCROLL_%s_世界\\n\' "$n"; done');
                await output('SCROLL_160_世界');
                await quiet('outputSettled', 2200);
                await page.evaluate(() => window.term.scrollLines(-12));
                await quiet('scrollback', 2200);
                await pixels('scrollback');
                await page.evaluate(() => window.term.scrollToBottom());
                await quiet('bottom', 2200);
                await pixels('scroll to bottom');

                await page.evaluate(() => {
                    window.term.options.fontWidth = 0.83;
                    window.term.options.letterSpacing = -0.3;
                    window.term.options.lineHeight = 1.31;
                    window.term.fit();
                });
                await quiet('fractionalFontChange');
                await pixels('fractional font change');
                await page.setViewportSize({ width: 1099, height: 733 });
                await quiet('windowResize');
                await pixels('window resize');

                await client.send('Page.setWebLifecycleState', { state: 'frozen' });
                const frozenBefore = await metrics();
                await new Promise(resolve => setTimeout(resolve, 800));
                const frozenAfter = await metrics();
                report.frozenTaskMs = Math.round((frozenAfter.TaskDuration - frozenBefore.TaskDuration) * 1000);
                await client.send('Page.setWebLifecycleState', { state: 'active' });
                await quiet('resumed');
                await pixels('resume');
                await page.evaluate(() => window.term.write('\x1b[4m   \x1b[9m   \x1b[0m\x1b[41m   \x1b[0m'));
                await quiet('decoratedSpaces');
                await pixels('decorated spaces');
                for (const style of ['bar', 'underline', 'block']) {
                    await page.evaluate(value => {
                        window.term.options.cursorStyle = value;
                    }, style);
                    await page.waitForTimeout(100);
                    await pixels(`cursor ${style}`);
                }

                await page.evaluate(() => window.term.write('\x1b[?1049h\x1b[2J\x1b[H\x1b[31mALT_BUFFER\x1b[0m'));
                await quiet('alternateScreen');
                await pixels('alternate screen');
                await page.evaluate(() => window.term.write('\x1b[?1049l'));
                await quiet('normalScreenRestored');
                await pixels('normal screen restored');

                await page.evaluate(() => {
                    window.term.reset();
                    window.term.write('ABCDEFGHIJKL\r\n');
                });
                await page.waitForTimeout(200);
                const grid = await page.evaluate(() => {
                    const rect = window.term.element.querySelector('canvas').getBoundingClientRect();
                    const metrics = window.term.renderer.getMetrics();
                    return { x: rect.x, y: rect.y, ...metrics };
                });
                await page.mouse.move(grid.x + grid.width * 0.2, grid.y + grid.height * 0.5);
                await page.mouse.down();
                await page.mouse.move(grid.x + grid.width * 8.2, grid.y + grid.height * 0.5, { steps: 8 });
                await page.mouse.up();
                assert(
                    (await page.evaluate(() => window.term.getSelection())).startsWith('ABCDEFGH'),
                    'mouse selection does not match the fractional grid'
                );
                await quiet('mouseSelection');
                await pixels('mouse selection');

                assert.deepEqual(errors, []);
                console.log(JSON.stringify(report));
            } finally {
                await context.close();
            }
        }
    } finally {
        if (browser) await browser.close();
        if (server.exitCode === null) {
            const closed = new Promise(resolve => server.once('exit', resolve));
            server.kill('SIGTERM');
            await closed;
        }
    }
}
main().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
