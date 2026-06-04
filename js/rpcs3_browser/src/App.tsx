import './App.css';
import { ChangeEvent, useState, useEffect, useRef } from 'react';
import { Rpcs3Module } from './Rpcs3Module';
import { useAppDispatch, useAppSelector, bootFile, init } from "./Actions";
import { getRunnableGame } from './Games';

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

  // End loading when real content is actually ON SCREEN. The signal is
  // getContentFrames() — the count of VARIED composited frames, which stays 0
  // through the all-black/navy boot and goes non-zero only once the game is
  // visible. (getFrames counts boot keep-alive presents; getFlipCount counts the
  // game's black boot flips — both fire too early, so neither works here.)
  useEffect(() => {
    if (done) return;
    const start = startRef.current;
    const id = setInterval(() => {
      const m = Rpcs3Module as { getContentFrames?: () => number } | undefined;
      const content = m && typeof m.getContentFrames === 'function' ? m.getContentFrames() : -1;
      const elapsed = (Date.now() - start) / 1000;
      // content > 0 → a real (non-black) frame is on screen. content < 0 → older
      // wasm without the accessor; fall back to a generous boot budget.
      if (content > 0 || (content < 0 && elapsed > 300)) {
        setDone(true);
      }
    }, 250);
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
  );
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

  const onPickFile = (e: ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (file) {
      dispatch(bootFile(file));
      setBootHidden(true);
    }
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
              accept=".iso,.bin,.pkg,.elf,.self"
              onChange={onPickFile}
              disabled={state.value !== 'initialized'}
            />
          </div>
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
