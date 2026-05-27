// Headless-Chrome screenshot capture for the RPCS3 WebGPU POC, adapted
// from Play-GamePlaytoo/js/play_browser/capture_screenshot.js.
//
// Differences from the PS2 original:
//   - Dev server port  3020 → 3030  (matches package.json's PORT=3030)
//   - URL pattern: drives ?loadFixedRom=true&game=<key> instead of
//     ?compare=<key>, since the rpcs3 POC's primary trigger is the
//     fixed-ROM autoboot path we just wired up. (?compare= still works
//     via Compare.tsx if you'd rather see the side-by-side view.)
//   - Trigger console message: Play! emitted "Emulator paused after fixed
//     state load" after its state-load thunk. rpcs3 has no state-load
//     path yet — for the POC we trigger on Actions.ts's bootFile fulfilled
//     message ("Loaded ROM" hasn't been wired; today the cleanest
//     observable signal is the redux state transition to 'loaded', which
//     PlayModule logs as "loaded"). We accept any of three candidate
//     strings so the script keeps working as the JS surface evolves.
//
// Usage:
//   node capture_screenshot.js <game_key> <output_name.png>
//   node capture_screenshot.js <game_key> <output_name.png> "ram=1024&webgpu=true"
//   node capture_screenshot.js <game_key> <output_name.png> "ram=1024" "JsDiscBridge: read"
//   (the 4th arg overrides the default trigger list with a single
//    substring — capture fires when that substring first appears in a
//    browser console message)
//
// Output path follows Play!'s convention — written to
// /Users/bank23232525/.gemini/antigravity/scratch/<output_name>.
const { exec } = require('child_process');
const http = require('http');
const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');

const game = process.argv[2];
const outputName = process.argv[3];

if (!game || !outputName) {
    console.error('Usage: node capture_screenshot.js <game_key> <output_name>');
    process.exit(1);
}

const extraParams = process.argv[4] ? `&${process.argv[4]}` : '';
const customTrigger = process.argv[5] || null;
// Optional 6th arg = URL mode: "compare" → /?compare=<game> instead of
// the default /?loadFixedRom=true&game=<game>. The compare URL renders
// the side-by-side iframe (live WebGPU left, reference image right),
// which is what the user actually sees when reviewing the POC; the
// loadFixedRom URL is the single-pane direct view used for automated
// boot tests.
const urlMode = process.argv[6] || null;
// The autoboot path: ?loadFixedRom=true picks the game's rom file from
// Games.ts (default key: 'demons' → TERRARIA → terraria.iso) and fetches
// it from http://localhost:8008. Run a static server in your ROM dir
// before invoking this script (see App.tsx's FIXED_ROM_BASE comment).
const targetUrl = urlMode === 'compare'
    ? `http://localhost:3030/?compare=${game}${extraParams}`
    : `http://localhost:3030/?loadFixedRom=true&game=${game}&ignore_log=false&autoInit=true${extraParams}`;
const screenshotPath = path.join('/Users/bank23232525/.gemini/antigravity/scratch', outputName);

console.log(`Target Game: ${game}`);
console.log(`Target URL:  ${targetUrl}`);
console.log(`Output Path: ${screenshotPath}`);
console.log('Launching headless Chrome...');

const profileDir = `/tmp/chrome-headless-profile-rpcs3-${Date.now()}`;
// --enable-unsafe-webgpu is required on stable Chrome until WebGPU graduates
// from "experimental" status on every channel. Harmless once it has.
const chromeCmd = [
    '"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"',
    '--headless=new',
    '--remote-debugging-port=9223',                // 9222 is the Play! port; offset to avoid clashes
    '--remote-debugging-address=127.0.0.1',
    '--enable-unsafe-webgpu',
    '--enable-features=Vulkan',                    // emdawnwebgpu prefers a Vulkan adapter on Linux/CI
    `--user-data-dir=${profileDir}`,
    `"${targetUrl}"`,
].join(' ');
console.log(`Command: ${chromeCmd}`);
const chromeProcess = exec(chromeCmd);

chromeProcess.stdout.on('data', (data) => console.log('CHROME STDOUT:', data));
chromeProcess.stderr.on('data', (data) => console.log('CHROME STDERR:', data));

chromeProcess.on('error', (err) => {
    console.error('Failed to launch Chrome:', err);
    process.exit(1);
});

function getWsUrl(retries = 30) {
    if (retries <= 0) {
        console.error('Failed to connect to Chrome debugging port.');
        chromeProcess.kill('SIGKILL');
        process.exit(1);
    }
    http.get('http://127.0.0.1:9223/json', (res) => {
        let data = '';
        res.on('data', chunk => data += chunk);
        res.on('end', () => {
            try {
                const list = JSON.parse(data);
                // Match on the URL substring — the param we care about
                // depends on URL mode: compare=<game> for the split-screen
                // view, game=<game> for the single-pane autoboot. Either
                // way the game key uniquely identifies our tab.
                const matchSubstr = urlMode === 'compare'
                    ? ('compare=' + game)
                    : ('game=' + game);
                const target = list.find(t => t.url.includes(matchSubstr));
                if (target && target.webSocketDebuggerUrl) {
                    console.log('Found target page. WebSocket URL:', target.webSocketDebuggerUrl);
                    connectToDebugger(target.webSocketDebuggerUrl);
                } else {
                    console.log('Target page not found yet, retrying...');
                    setTimeout(() => getWsUrl(retries - 1), 1000);
                }
            } catch (e) {
                console.log('Parsing target list failed, retrying...');
                setTimeout(() => getWsUrl(retries - 1), 1000);
            }
        });
    }).on('error', (err) => {
        console.log('HTTP error querying targets:', err.message);
        setTimeout(() => getWsUrl(retries - 1), 1000);
    });
}

function captureNow(ws) {
    console.log('Sending Page.captureScreenshot request...');
    ws.send(JSON.stringify({ id: 4, method: 'Page.captureScreenshot', params: { format: 'png' } }));
}

function connectToDebugger(wsUrl) {
    const ws = new WebSocket(wsUrl);
    let captured = false;

    ws.on('open', () => {
        console.log('Connected to debugger WebSocket.');
        ws.send(JSON.stringify({ id: 1, method: 'Runtime.enable' }));
        ws.send(JSON.stringify({ id: 2, method: 'Log.enable' }));
        ws.send(JSON.stringify({ id: 3, method: 'Page.enable' }));
        ws.send(JSON.stringify({ id: 5, method: 'Network.enable' }));
        ws.send(JSON.stringify({ id: 6, method: 'Network.setCacheDisabled', params: { cacheDisabled: true } }));
    });

    ws.on('message', (message) => {
        try {
            const obj = JSON.parse(message);

            if (obj.method === 'Runtime.consoleAPICalled') {
                const type = obj.params.type.toUpperCase();
                const args = obj.params.args.map(arg => {
                    if (arg.value !== undefined) return arg.value;
                    if (arg.description !== undefined) return arg.description;
                    return JSON.stringify(arg);
                }).join(' ');
                console.log(`[BROWSER CONSOLE ${type}] ${args}`);

                // Capture-trigger candidates (any of these means the boot
                // has progressed far enough to be worth snapshotting).
                // Ordered roughly "latest milestone first" — the FIRST
                // match wins, so listing later phases earlier means the
                // capture waits for them when the build has progressed.
                //   - "JsDiscBridge: read"     — Phase 2 bridge proved bytes round-trip
                //   - "loaded ROM"             — Actions.ts bootFile.fulfilled
                //   - "BootDiscImage"          — Rpcs3VmJs.cpp printf
                //   - "initVmWebGPU completed" — WebGPU surface up (Phase 0/1 baseline)
                const triggers = customTrigger
                    ? [customTrigger]
                    : [
                          'JsDiscBridge: read',
                          'loaded ROM',
                          'BootDiscImage',
                          'initVmWebGPU completed',
                      ];
                if (!captured && triggers.some(t => args.includes(t))) {
                    captured = true;
                    console.log('Detected boot trigger. Capturing screenshot in 1500ms...');
                    setTimeout(() => captureNow(ws), 1500);
                }
            }

            if (obj.method === 'Runtime.exceptionThrown') {
                const details = obj.params.exceptionDetails;
                const msg = details.exception ? (details.exception.description || details.exception.value) : details.text;
                console.error(`[BROWSER UNCAUGHT EXCEPTION] ${msg}`);
            }

            if (obj.id === 4) {
                if (obj.result && obj.result.data) {
                    const buffer = Buffer.from(obj.result.data, 'base64');
                    fs.mkdirSync(path.dirname(screenshotPath), { recursive: true });
                    fs.writeFileSync(screenshotPath, buffer);
                    console.log('Screenshot saved to:', screenshotPath);
                } else {
                    console.error('Screenshot capture failed:', obj);
                }
                ws.close();
                chromeProcess.kill('SIGKILL');
                process.exit(0);
            }
        } catch (e) {
            // Ignore parse errors
        }
    });

    ws.on('close', () => {
        console.log('Debugger connection closed.');
        chromeProcess.kill('SIGKILL');
        process.exit(1);
    });

    // Fallback capture: even with no recognised trigger console message,
    // snapshot what's on screen after 60 s so the script always produces
    // SOMETHING (useful while the C++ side is still printf-stubs and
    // hasn't started logging the trigger strings yet).
    setTimeout(() => {
        if (!captured) {
            captured = true;
            console.log('No trigger seen in 60s — capturing whatever is on screen.');
            captureNow(ws);
        }
    }, 60000);

    // Hard timeout: 3 minutes max, then bail.
    setTimeout(() => {
        console.error('Timeout of 180 seconds exceeded! Crashing...');
        chromeProcess.kill('SIGKILL');
        process.exit(1);
    }, 180000);
}

setTimeout(() => getWsUrl(), 1000);
