// Timelapse variant of capture_screenshot.js — after the boot trigger fires,
// capture a screenshot every <intervalMs> for <totalMs>, saving as
// <base>_NNN.png. Lets us watch the splash -> menu transition instead of
// grabbing a single final frame.
//
// Usage:
//   node capture_timelapse.js <game_key> <base_name> "<params>" "<trigger>" "<urlMode>" <intervalMs> <totalMs>
//   HEADLESS=0 node capture_timelapse.js terraria run123 "ram=2048" "" "" 20000 360000

const http = require('http');
const fs = require('fs');
const path = require('path');
const { exec } = require('child_process');
const WebSocket = require('ws');

const game = process.argv[2];
const baseName = process.argv[3];
if (!game || !baseName) {
    console.error('Usage: node capture_timelapse.js <game_key> <base_name> <params> <trigger> <urlMode> <intervalMs> <totalMs>');
    process.exit(1);
}
const extraParams = process.argv[4] ? `&${process.argv[4]}` : '';
const customTrigger = process.argv[5] || null;
const urlMode = process.argv[6] || null;
const intervalMs = process.argv[7] ? parseInt(process.argv[7], 10) : 20000;
const totalMs = process.argv[8] ? parseInt(process.argv[8], 10) : 360000;

const targetUrl = urlMode === 'compare'
    ? `http://localhost:3030/?compare=${game}${extraParams}`
    : `http://localhost:3030/?loadFixedRom=true&game=${game}&ignore_log=false&autoInit=true${extraParams}`;
const scratch = '/Users/bank23232525/.gemini/antigravity/scratch';

console.log(`Target Game: ${game}`);
console.log(`Target URL:  ${targetUrl}`);
console.log(`Timelapse:   every ${intervalMs}ms for ${totalMs}ms -> ${scratch}/${baseName}_NNN.png`);

const profileDir = `/tmp/chrome-headless-profile-rpcs3-${Date.now()}`;
const chromeCmd = [
    '"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"',
    (process.env.HEADLESS === '0' ? '' : '--headless=new'),
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-backgrounding-occluded-windows',
    '--disable-renderer-backgrounding',
    '--disable-background-timer-throttling',
    '--remote-debugging-port=9223',
    '--remote-debugging-address=127.0.0.1',
    '--enable-unsafe-webgpu',
    `--user-data-dir=${profileDir}`,
    `"${targetUrl}"`,
].filter(Boolean).join(' ');
console.log(`Command: ${chromeCmd}`);
const chromeProcess = exec(chromeCmd);
chromeProcess.stdout.on('data', (data) => console.log('CHROME STDOUT:', data));
chromeProcess.stderr.on('data', (data) => console.log('CHROME STDERR:', data));
chromeProcess.on('error', (err) => { console.error('Failed to launch Chrome:', err); process.exit(1); });

let shotCounter = 0;          // global across reconnects
let triggered = false;        // boot trigger seen
let intervalHandle = null;
let startedAt = 0;

function pad(n) { return String(n).padStart(3, '0'); }

function startTimelapse(ws) {
    if (intervalHandle) return;
    startedAt = Date.now();
    console.log(`Boot trigger seen — starting timelapse.`);
    const shoot = () => {
        const id = 1000 + shotCounter;
        ws.send(JSON.stringify({ id, method: 'Page.captureScreenshot', params: { format: 'png' } }));
    };
    shoot(); // immediate first shot
    intervalHandle = setInterval(() => {
        if (Date.now() - startedAt >= totalMs) {
            console.log('Timelapse complete.');
            try { ws.close(); } catch (e) {}
            chromeProcess.kill('SIGKILL');
            process.exit(0);
        }
        shoot();
    }, intervalMs);
}

function getWsUrl(retries = 30) {
    if (retries <= 0) { console.error('Failed to connect to debugging port.'); chromeProcess.kill('SIGKILL'); process.exit(1); }
    http.get('http://127.0.0.1:9223/json', (res) => {
        let data = '';
        res.on('data', chunk => data += chunk);
        res.on('end', () => {
            try {
                const list = JSON.parse(data);
                const matchSubstr = urlMode === 'compare' ? ('compare=' + game) : ('game=' + game);
                const target = list.find(t => t.url.includes(matchSubstr));
                if (target && target.webSocketDebuggerUrl) {
                    connectToDebugger(target.webSocketDebuggerUrl);
                } else { setTimeout(() => getWsUrl(retries - 1), 1000); }
            } catch (e) { setTimeout(() => getWsUrl(retries - 1), 1000); }
        });
    }).on('error', () => setTimeout(() => getWsUrl(retries - 1), 1000));
}

let reconnectCount = 0;
function connectToDebugger(wsUrl) {
    const ws = new WebSocket(wsUrl);
    ws.on('open', () => {
        ws.send(JSON.stringify({ id: 1, method: 'Runtime.enable' }));
        ws.send(JSON.stringify({ id: 2, method: 'Log.enable' }));
        ws.send(JSON.stringify({ id: 3, method: 'Page.enable' }));
        // If we already saw the trigger (reconnect mid-timelapse), resume shooting.
        if (triggered) { intervalHandle = null; startTimelapse(ws); }
    });
    ws.on('message', (message) => {
        try {
            const obj = JSON.parse(message);
            if (obj.method === 'Runtime.consoleAPICalled') {
                const type = obj.params.type.toUpperCase();
                const args = obj.params.args.map(a => a.value !== undefined ? a.value : (a.description !== undefined ? a.description : '')).join(' ');
                console.log(`[BROWSER CONSOLE ${type}] ${args}`);
                const triggers = customTrigger ? [customTrigger] : ['JsDiscBridge: read', 'loaded ROM', 'BootDiscImage', 'initVmWebGPU completed'];
                if (!triggered && triggers.some(t => args.includes(t))) {
                    triggered = true;
                    startTimelapse(ws);
                }
            }
            if (obj.id !== undefined && obj.id >= 1000) {
                const shot = obj.id - 1000;
                if (obj.result && obj.result.data) {
                    const p = path.join(scratch, `${baseName}_${pad(shot)}.png`);
                    fs.mkdirSync(path.dirname(p), { recursive: true });
                    fs.writeFileSync(p, Buffer.from(obj.result.data, 'base64'));
                    const t = ((Date.now() - startedAt) / 1000).toFixed(0);
                    console.log(`[TIMELAPSE] shot ${pad(shot)} saved @${t}s -> ${p}`);
                    shotCounter++;
                } else {
                    console.error('Screenshot failed:', JSON.stringify(obj).slice(0, 120));
                    shotCounter++; // advance anyway so numbering keeps moving
                }
            }
        } catch (e) {}
    });
    ws.on('close', () => {
        if (reconnectCount < 120) {
            reconnectCount++;
            console.log(`CDP closed — reconnecting #${reconnectCount} (page kept alive)...`);
            if (intervalHandle) { clearInterval(intervalHandle); intervalHandle = null; }
            setTimeout(() => getWsUrl(), 1500);
        } else {
            chromeProcess.kill('SIGKILL'); process.exit(0);
        }
    });
    ws.on('error', (e) => console.log('WS error:', e.message));
}

getWsUrl();
