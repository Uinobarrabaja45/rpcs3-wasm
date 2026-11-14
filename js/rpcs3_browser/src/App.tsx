import './App.css';
import { ChangeEvent, useState, useEffect, useRef } from 'react';
import { Rpcs3Module } from './Rpcs3Module';
import { useAppDispatch, useAppSelector, bootFile, init } from "./Actions";
import { getRunnableGame } from './Games';
import Ps3zReader from './Ps3zReader';
import Ps3zDevice from './Ps3zDevice';

// Fixed-ROM fileserver — matches Play!'s `loadFixedRomAndState=true` flow
// but on port 8008 (Play! uses :3000 for its CI server). Run a local static
// server in the directory that holds your test ISOs, e.g.:
//   cd ~/roms && python3 -m http.server 8008
// then open the React app with ?loadFixedRom=true.
const FIXED_ROM_BASE = 'http://localhost:8008';

function Stats() {
  const [fps, setFps] = useState(0);
  useEffect(() => {
    const timer = setInterval(() => {
      if (Rpcs3Module && typeof Rpcs3Module.getFrames === 'function') {
        const frames = Rpcs3Module.getFrames();
        if (typeof Rpcs3Module.clearStats === 'function') Rpcs3Module.clearStats();
        setFps(frames);
      }
    }, 1000);
    return () => clearInterval(timer);
  }, []);
  return (
    <div className="fps-pill" aria-live="polite" aria-label={`${fps} frames per second`}>
      <span className="fps-pill__num">{fps}</span>
      <span className="fps-pill__label">FPS</span>
    </div>
  );
}

// Loading gauge shown over the (blank) canvas from boot-start until the game is
// actually visible. There is no real progress signal during the ~2-3 min PS3
// boot, so the bar trickles with a smooth asymptotic curve toward ~95% and snaps
// to 100% the instant getContentFrames() goes non-zero (the count of varied,
// real-content composited frames, Main.cpp), then fades out.
function LoadingGauge() {
  const [progress, setProgress] = useState(0);
  const [done, setDone] = useState(false);
  const [unmount, setUnmount] = useState(false);
  const startRef = useRef<number>(Date.now());

  // End loading only when the SCENE VISUALLY STABILIZES — not at the first content
  // frame. The title screen cycles violently between different biome renders for
  // minutes while loading (the real "flashy" the user sees). getContentFrames /
  // color-variance can't detect it (every cycling scene is high-variance), so we
  // sample the canvas directly and watch frame-to-frame DIFFERENCE: large while
  // cycling, near-zero once it settles. Hide the gauge after it holds steady.
  useEffect(() => {
    if (done) return;
    const start = startRef.current;
    const W = 32, H = 18;
    const tmp = document.createElement('canvas'); tmp.width = W; tmp.height = H;
    const ctx = tmp.getContext('2d', { willReadFrequently: true });
    let prev: Uint8ClampedArray | null = null;
    let steady = 0;
    const id = setInterval(() => {
      const elapsed = (Date.now() - start) / 1000;
      const cv = document.getElementById('outputCanvas') as HTMLCanvasElement | null;
      if (!cv || !ctx) { if (elapsed > 420) setDone(true); return; }
      let cur: Uint8ClampedArray;
      try { ctx.drawImage(cv, 0, 0, W, H); cur = ctx.getImageData(0, 0, W, H).data; }
      catch { if (elapsed > 420) setDone(true); return; }
      let sum = 0; for (let i = 0; i < cur.length; i += 4) sum += cur[i] + cur[i + 1] + cur[i + 2];
      const mean = sum / (W * H * 3);
      if (mean < 8) { steady = 0; prev = cur; if (elapsed > 420) setDone(true); return; } // still black/booting
      if (prev) {
        let diff = 0;
        for (let i = 0; i < cur.length; i += 4) diff += Math.abs(cur[i] - prev[i]) + Math.abs(cur[i + 1] - prev[i + 1]) + Math.abs(cur[i + 2] - prev[i + 2]);
        const avg = diff / (W * H * 3);
        if (avg < 6) steady++; else steady = 0; // settled scene vs biome-cycling
      }
      prev = cur;
      // Settled ~1.5s steady → reveal the stable title. Hard 7-min cap as a backstop.
      if (steady >= 7 || elapsed > 420) setDone(true);
    }, 220);
    return () => clearInterval(id);
  }, [done]);

  // Trickle the bar (no real progress until the first frame), then snap to 100%
  // and fade out once rendering starts.
  useEffect(() => {
    if (done) {
      setProgress(100);
      const t = setTimeout(() => setUnmount(true), 650);
      return () => clearTimeout(t);
    }
    const id = setInterval(() => {
      const elapsed = (Date.now() - startRef.current) / 1000;
      setProgress(95 * (1 - Math.exp(-elapsed / 55))); // ~63% @1min, ~86% @2min, never reaches 95
    }, 150);
    return () => clearInterval(id);
  }, [done]);

  if (unmount) return null;
  const pct = Math.round(Math.min(100, progress));
  return (
    <>
      {/* Opaque full-screen backdrop hides the biome-cycling canvas behind the gauge until settled. */}
      <div className={`loading-backdrop${done ? ' loading-backdrop--done' : ''}`} aria-hidden="true" />
      <div
        className={`loading-gauge${done ? ' loading-gauge--done' : ''}`}
      role="progressbar"
      aria-valuenow={pct}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-label="Loading game"
    >
      <div className="loading-gauge__spinner" aria-hidden="true" />
      <div className="loading-gauge__label">{done ? 'Ready' : 'Booting PS3 — loading game…'}</div>
      <div className="loading-gauge__track">
        <div className="loading-gauge__fill" style={{ width: `${pct}%` }} />
      </div>
      <div className="loading-gauge__pct">{pct}%</div>
      </div>
    </>
  );
}

// Shows the wasm's compile time (getBuildTag) in a corner. Its very presence means
// the browser loaded the freshly-built wasm — a stale/old wasm has no getBuildTag,
// so the tag stays hidden. Lets the user confirm they're not on a cached old build.
function BuildTag() {
  const [tag, setTag] = useState<string | null>(null);
  useEffect(() => {
    const id = setInterval(() => {
      const m = Rpcs3Module as { getBuildTag?: () => string } | undefined;
      if (m && typeof m.getBuildTag === 'function') {
        try { const t = m.getBuildTag(); if (t) { setTag(t); clearInterval(id); } } catch { /* not ready */ }
      }
    }, 500);
    return () => clearInterval(id);
  }, []);
  if (!tag) return null;
  return <div className="build-tag">wasm&nbsp;{tag}</div>;
}

function App() {
  const state = useAppSelector((s) => s.rpcs3);
  const dispatch = useAppDispatch();

  const urlParams = new URLSearchParams(window.location.search);
  const showFPS = urlParams.get('showFPS') !== 'false';
  const autoInit = urlParams.get('autoInit') !== 'false';
  // ?loadFixedRom=true — fetch the active game's ROM from a local fileserver
  // and boot it without the user picking a file. Used for compare-mode and
  // automated screenshot/CI runs (matches Play!'s ?loadFixedRomAndState=true
  // pattern). The active game is chosen by ?game=<key>, falling back to the
  // default in Games.ts (TERRARIA). The ROM filename comes from the game's
  // `rom` field, so ?game=terraria fetches FIXED_ROM_BASE + '/terraria.iso'.
  const loadFixedRom = urlParams.get('loadFixedRom') === 'true';
  const fixedGameKey = urlParams.get('game');

  const [bootHidden, setBootHidden] = useState(false);
  const [fixedRomTriggered, setFixedRomTriggered] = useState(false);
  const [ps3zMessage, setPs3zMessage] = useState('');

  // Auto-initialize the WASM module on mount unless explicitly disabled.
  // The Play! flow used dispatch(init()) from index.tsx; here we do the
  // same but defer until after the React tree is mounted so the canvas
  // exists in the DOM before initVmWebGPU goes looking for it.
  useEffect(() => {
    if (autoInit && state.value === 'unknown') {
      dispatch(init());
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Once the WASM is initialized, fire the fixed-ROM fetch+boot exactly
  // once. Guarded by `fixedRomTriggered` because state.value transitions
  // unknown → initialized → loaded and we only want to act on the first
  // edge into 'initialized'.
  useEffect(() => {
    if (!loadFixedRom) return;
    if (state.value !== 'initialized') return;
    if (fixedRomTriggered) return;
    setFixedRomTriggered(true);

    (async () => {
      const game = getRunnableGame(fixedGameKey);
      const url = `${FIXED_ROM_BASE}/${game.rom}`;
      console.log(`[App] loadFixedRom — fetching ${url}`);
      try {
        const response = await fetch(url);
        if (!response.ok) {
          console.error(`[App] loadFixedRom failed: HTTP ${response.status} for ${url}`);
          return;
        }
        // ISOs are large but we don't stream the network fetch here — the
        // browser holds the whole response in memory, then we wrap it in a
        // File so DiscImageDevice can slice() it the same way as a
        // user-picked file. If this becomes a memory problem for retail
        // BD-ROMs (25 GB+), switch to a Range-request-backed File-like
        // shim instead of a real File.
        const blob = await response.blob();
        const file = new File([blob], game.rom, { type: 'application/octet-stream' });
        console.log(`[App] loadFixedRom — fetched ${file.size} bytes, dispatching bootFile`);
        dispatch(bootFile(file));
        setBootHidden(true);
      } catch (e) {
        console.error('[App] loadFixedRom fetch threw:', e);
      }
    })();
  }, [state.value, loadFixedRom, fixedRomTriggered, fixedGameKey, dispatch]);

  const onPickFile = async (e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;

    const extension = file.name.slice(file.name.lastIndexOf('.')).toLowerCase();

    if (extension === '.ps3z') {
      setPs3zMessage('Comprobando archivo PS3Z...');


      try {
        const reader = await Ps3zReader.open(file);

        // Registrar el dispositivo PS3Z en el objeto JS del módulo WASM.
        // Esto prepara el lado JavaScript; todavía no conecta el VFS de C++.
        if (!Rpcs3Module || !Rpcs3Module.HEAPU8) {
          throw new Error('El módulo WASM no está preparado.');
        }

        const ps3zDevice = new Ps3zDevice(Rpcs3Module, reader);
        Rpcs3Module.ps3zDevice = ps3zDevice;

        const rootInfo = ps3zDevice.getPathInfo('');
        console.log('[PS3Z] Dispositivo JS registrado:', {
          hasBundle: ps3zDevice.hasBundle(),
          root: rootInfo,
        });
        const eboots = reader.findFilesByName('EBOOT.BIN');

        const normalizePath = (path: string) =>
          path.replace(/\\/g, '/').replace(/^\/+/, '');

        const normalizedEboots = eboots.map(entry => ({
          path: normalizePath(entry.path),
        }));

        // Preferimos la ubicación habitual de un juego de PS3.
        const selectedEboot =
          normalizedEboots.find(item =>
            /(^|\/)PS3_GAME\/USRDIR\/EBOOT\.BIN$/i.test(item.path)
          ) ??
          normalizedEboots.find(item =>
            /(^|\/)USRDIR\/EBOOT\.BIN$/i.test(item.path)
          ) ??
          normalizedEboots.find(item =>
            /(^|\/)EBOOT\.BIN$/i.test(item.path)
          );

        let expectedParamSfo = '';
        let hasParamSfo = false;

        if (selectedEboot) {
          const ebootPath = selectedEboot.path;
          const lowerPath = ebootPath.toLowerCase();
          const suffix = 'usrdir/eboot.bin';
          const suffixIndex = lowerPath.lastIndexOf(suffix);

          let gameDirectory = '';

          if (
            suffixIndex >= 0 &&
            (suffixIndex === 0 || lowerPath[suffixIndex - 1] === '/')
          ) {
            gameDirectory = ebootPath.slice(0, suffixIndex).replace(/\/$/, '');
          } else {
            const lastSlash = ebootPath.lastIndexOf('/');
            gameDirectory = lastSlash >= 0
              ? ebootPath.slice(0, lastSlash)
              : '';
          }

          expectedParamSfo = gameDirectory
            ? `${gameDirectory}/PARAM.SFO`
            : 'PARAM.SFO';

          hasParamSfo = !!reader.getFile(expectedParamSfo);
        }

        const ebootMessage = eboots.length > 0
          ? eboots.slice(0, 10).map(entry => entry.path).join('\n')
          : 'No se ha encontrado EBOOT.BIN.';

        setPs3zMessage(
          'PS3Z reconocido. Índice verificado.\n' +
          'Carpeta: ' + reader.rootName + '\n' +
          'Archivos: ' + reader.totalFiles + '\n' +
          'Tamaño: ' + (reader.totalBytes / 1024 / 1024 / 1024).toFixed(2) + ' GB\n' +
          'EBOOT.BIN encontrados: ' + eboots.length + '\n' +
          'EBOOT seleccionada: ' + (selectedEboot?.path || 'No encontrada') + '\n' +
          'PARAM.SFO: ' + (hasParamSfo ? 'Encontrado' : 'No encontrado') + '\n' +
          'Ruta PARAM.SFO esperada: ' + (expectedParamSfo || 'No determinada') + '\n\n' +
          ebootMessage +
          '\n\nTodavía no se ha iniciado el juego: falta conectar el dispositivo virtual PS3Z.'
        );
      } catch (error) {
        setPs3zMessage(
          'Error al comprobar PS3Z: ' +
          (error instanceof Error ? error.message : String(error))
        );
      }

      return;
    }

    setPs3zMessage('');
    dispatch(bootFile(file));
    setBootHidden(true);
  };

  const onBootXmb = () => {
    // No file — try the XMB. The C++ side will refuse if no firmware is
    // installed, but it's a handy smoke test for the init path.
    if (Rpcs3Module && typeof Rpcs3Module.bootXmb === 'function') {
      Rpcs3Module.bootXmb();
      setBootHidden(true);
    } else {
      console.warn('Rpcs3Module.bootXmb not available — WASM module not ready.');
    }
  };

  const showPanel = !bootHidden && state.value !== 'loaded';

  return (
    <div className="App">
      <BuildTag />
      {showFPS && state.value === 'loaded' && <Stats />}
      {bootHidden && <LoadingGauge />}
      {showPanel && (
        <div className="boot-panel">
          <h2>RPCS3 — WebGPU PS3 emulator (POC)</h2>
          <div style={{ marginBottom: 12, opacity: 0.75, fontSize: 13 }}>
            Status: {state.value}
          </div>
          <div>
            <input
              type="file"
              accept=".iso,.bin,.pkg,.elf,.self,.ps3z"
              onChange={onPickFile}
              disabled={state.value !== 'initialized'}
            />
          </div>

          {ps3zMessage && (
            <pre style={{
              marginTop: 12,
              padding: 12,
              background: '#222',
              color: '#fff',
              whiteSpace: 'pre-wrap',
              overflowWrap: 'anywhere',
              fontSize: 12
            }}>
              {ps3zMessage}
            </pre>
          )}

          <div style={{ marginTop: 12 }}>
            <button onClick={onBootXmb} disabled={state.value !== 'initialized'}>
              Boot XMB
            </button>
          </div>
          <div style={{ marginTop: 10, fontSize: 11, opacity: 0.6 }}>
            URL params: ?ram=512|1024|2048|dyna · ?webgpu=false · ?hd=false
            · ?renderRes=N · ?showFPS=false · ?mute=true
            · ?loadFixedRom=true&amp;game=&lt;key&gt; (fetches from {FIXED_ROM_BASE})
          </div>
        </div>
      )}
    </div>
  );
}

export default App;
