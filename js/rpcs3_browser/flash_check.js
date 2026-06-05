// Live flash detector. Boots a game headless (unthrottled rAF), waits for the
// menu, then samples the canvas at full frame rate and reports per-frame color
// STDDEV (varied menu = high, uniform navy backbuffer = low). 12s timelapse
// screenshots alias the menu<->navy flash and look stable; this catches it.
//
//   node flash_check.js <game_key> "<params>"
//   HEADLESS=0 node flash_check.js terraria "ram=2048"

const http = require('http');
const { exec } = require('child_process');
const WebSocket = require('ws');

const game = process.argv[2] || 'terraria';
const extraParams = process.argv[3] ? `&${process.argv[3]}` : '';
const targetUrl = `http://localhost:3030/?loadFixedRom=true&game=${game}&ignore_log=false&autoInit=true${extraParams}`;

const profileDir = `/tmp/chrome-flashcheck-${Date.now()}`;
const chromeCmd = [
    '"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"',
    (process.env.HEADLESS === '0' ? '' : '--headless=new'),
    '--no-first-run', '--no-default-browser-check',
    '--disable-backgrounding-occluded-windows',
    '--disable-renderer-backgrounding',
    '--disable-background-timer-throttling',
    '--remote-debugging-port=9224',
    '--remote-debugging-address=127.0.0.1',
    '--enable-unsafe-webgpu',
    `--user-data-dir=${profileDir}`,
    `"${targetUrl}"`,
].filter(Boolean).join(' ');
console.log('URL:', targetUrl);
const chromeProcess = exec(chromeCmd);
chromeProcess.on('error', (err) => { console.error('Chrome launch failed:', err); process.exit(1); });

// In-page sampler: N frames at rAF, per-frame downsampled color stddev + mean.
const SAMPLER = (n) => `(async () => {
  const cv = document.getElementById('outputCanvas');
  if (!cv) return JSON.stringify({err:'no canvas'});
  const w=96,h=54, tmp=document.createElement('canvas'); tmp.width=w; tmp.height=h;
  const ctx=tmp.getContext('2d',{willReadFrequently:true});
  const out=[];
  await new Promise(res=>{ let i=0; function tick(){
    try{ ctx.drawImage(cv,0,0,w,h); const d=ctx.getImageData(0,0,w,h).data; const np=w*h;
      let sR=0,sG=0,sB=0,sR2=0,sG2=0,sB2=0;
      for(let p=0;p<d.length;p+=4){const R=d[p],G=d[p+1],B=d[p+2]; sR+=R;sG+=G;sB+=B; sR2+=R*R;sG2+=G*G;sB2+=B*B;}
      const mR=sR/np,mG=sG/np,mB=sB/np;
      const vR=sR2/np-mR*mR, vG=sG2/np-mG*mG, vB=sB2/np-mB*mB;
      const sd=(Math.sqrt(Math.max(0,vR))+Math.sqrt(Math.max(0,vG))+Math.sqrt(Math.max(0,vB)))/3;
      out.push({sd:Math.round(sd), m:Math.round((mR+mG+mB)/3)});
    }catch(e){ out.push({err:String(e).slice(0,40)}); }
    if(++i<${n}) requestAnimationFrame(tick); else res();
  } requestAnimationFrame(tick); });
  return JSON.stringify(out);
})()`;

let evalId = 5000;
const pending = {};
function evalInPage(ws, expr, awaitPromise = false) {
    return new Promise((resolve) => {
        const id = ++evalId; pending[id] = resolve;
        ws.send(JSON.stringify({ id, method: 'Runtime.evaluate', params: { expression: expr, awaitPromise, returnByValue: true } }));
    });
}

function getWsUrl(retries = 40) {
    if (retries <= 0) { console.error('No debugging port.'); chromeProcess.kill('SIGKILL'); process.exit(1); }
    http.get('http://127.0.0.1:9224/json', (res) => {
        let data = ''; res.on('data', c => data += c);
        res.on('end', () => {
            try {
                const t = JSON.parse(data).find(t => t.url.includes('game=' + game));
                if (t && t.webSocketDebuggerUrl) run(t.webSocketDebuggerUrl);
                else setTimeout(() => getWsUrl(retries - 1), 1000);
            } catch (e) { setTimeout(() => getWsUrl(retries - 1), 1000); }
        });
    }).on('error', () => setTimeout(() => getWsUrl(retries - 1), 1000));
}

function run(wsUrl) {
    const ws = new WebSocket(wsUrl);
    ws.on('open', async () => {
        ws.send(JSON.stringify({ id: 1, method: 'Runtime.enable' }));
        console.log('connected; waiting for menu (polling canvas)...');
        const t0 = Date.now();
        let menu = false;
        // Poll one frame every 2.5s until a VARIED frame appears (menu up).
        while (Date.now() - t0 < 360000) {
            await new Promise(r => setTimeout(r, 2500));
            const r = await evalInPage(ws, SAMPLER(1), true);
            const val = r && r.result && r.result.result ? r.result.result.value : undefined;
            let v = null; try { v = JSON.parse(val); } catch (e) { console.log('  (eval parse fail; raw=', JSON.stringify(r && r.result).slice(0, 160), ')'); continue; }
            const s = Array.isArray(v) ? v[0] : v;
            const secs = ((Date.now() - t0) / 1000).toFixed(0);
            console.log(`  @${secs}s sample sd=${s && s.sd}, m=${s && s.m}`);
            if (s && s.sd != null && s.sd > 18) { menu = true; break; }
        }
        if (!menu) { console.log('MENU NEVER APPEARED (timeout)'); ws.close(); chromeProcess.kill('SIGKILL'); process.exit(2); }
        console.log('menu detected — sampling 90 frames at full rate...');
        const r = await evalInPage(ws, SAMPLER(90), true);
        const val = r && r.result && r.result.result ? r.result.result.value : undefined;
        let arr; try { arr = JSON.parse(val); } catch (e) { console.log('sample parse fail; raw=', JSON.stringify(r && r.result).slice(0, 200)); process.exit(3); }
        if (!Array.isArray(arr) || arr.some(x => x.err)) { console.log('canvas read errors:', JSON.stringify(arr).slice(0, 200)); }
        const sds = arr.map(x => x.sd).filter(x => x != null);
        const navy = sds.filter(x => x < 12).length;   // uniform navy/black frame
        const varied = sds.filter(x => x > 18).length;  // real menu frame
        const min = Math.min(...sds), max = Math.max(...sds);
        console.log('--- FLASH CHECK ---');
        console.log('frames sampled:', sds.length);
        console.log('per-frame stddev: min=' + min + ' max=' + max);
        console.log('navy/uniform frames (sd<12):', navy);
        console.log('varied/menu frames (sd>18):', varied);
        console.log('sequence (sd):', sds.join(','));
        const flashing = navy >= 5 && varied >= 5;
        console.log('VERDICT:', flashing ? 'FLASHING (menu<->navy alternation)' : (varied >= sds.length * 0.8 ? 'STABLE (menu held, no flash)' : 'INCONCLUSIVE'));
        // Grab a menu screenshot for visual confirmation (PRESS START / look).
        const shot = await new Promise((resolve) => { const id = ++evalId; pending[id] = resolve; ws.send(JSON.stringify({ id, method: 'Page.captureScreenshot', params: { format: 'png' } })); });
        try {
            const data = shot && shot.result && shot.result.data;
            if (data) { require('fs').writeFileSync('/Users/bank23232525/.gemini/antigravity/scratch/flashcheck_menu.png', Buffer.from(data, 'base64')); console.log('saved menu screenshot -> flashcheck_menu.png'); }
            else { console.log('screenshot failed:', JSON.stringify(shot && shot.result).slice(0, 120)); }
        } catch (e) { console.log('screenshot err', e.message); }
        ws.close(); chromeProcess.kill('SIGKILL'); process.exit(flashing ? 1 : 0);
    });
    ws.on('message', (m) => {
        try { const o = JSON.parse(m); if (o.id && pending[o.id]) { pending[o.id](o); delete pending[o.id]; } } catch (e) {}
    });
    ws.on('error', (e) => console.log('WS error:', e.message));
}
getWsUrl();
