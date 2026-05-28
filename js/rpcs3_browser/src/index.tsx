import React from 'react';
import ReactDOM from 'react-dom';
import './index.css';
import App from './App';
import Compare from './Compare';
import reportWebVitals from './reportWebVitals';
import { Provider } from "react-redux";
import { store } from "./Actions";

// `ignore_log` flag — default TRUE. Silences info console output from the
// emulator glue and UI. WASM-side printf is silenced separately in
// Rpcs3Module.ts. console.warn / console.error remain intact.
if (new URLSearchParams(window.location.search).get('ignore_log') !== 'false') {
  const noop = () => {};
  console.log = noop;
  console.info = noop;
  console.debug = noop;
}

// Filter Emscripten's libc-stub warnings — "warning: unsupported syscall:
// __syscall_mprotect/mlock/madvise/getrusage" fires hundreds of times per
// boot because rpcs3's vm layer calls these on every page protection or
// memory-hint operation. Emscripten's stubs return success, so the calls
// are pure noise but they bury real errors. Override BEFORE any pthread
// worker loads — pthread Module.printErr is per-worker so a Rpcs3Module
// override wouldn't apply to them; this console.warn override covers all.
{
  const SYSCALL_NOISE_RE = /^warning: unsupported syscall: __syscall_/;
  const origWarn = console.warn.bind(console);
  console.warn = (...args: any[]) => {
    if (args.length > 0 && typeof args[0] === 'string' && SYSCALL_NOISE_RE.test(args[0])) {
      return;
    }
    origWarn(...args);
  };
}

// Hide the webpack-dev-server full-screen error overlay. PPU/SPU pthread
// workers abort with wasm32 runtime errors (see Phase 4 below) and that
// surfaces a full-screen iframe overlay that covers the live canvas in
// dev mode. The errors are still in the console for debugging.
{
  const style = document.createElement('style');
  style.textContent = `
    #webpack-dev-server-client-overlay,
    body > iframe[style*="position: fixed"],
    body > iframe[id*="webpack-dev-server"],
    body > div[style*="position: fixed"][style*="z-index: 2147483647"] {
      display: none !important;
    }
  `;
  document.head.appendChild(style);
  // The overlay iframe is also inserted dynamically; remove it as it appears.
  new MutationObserver(() => {
    document.querySelectorAll('iframe').forEach(f => {
      const sel = (f.getAttribute('style') || '') + (f.id || '');
      if (sel.includes('position: fixed') || sel.includes('webpack-dev-server') || sel.includes('iframe-error-overlay')) {
        f.remove();
      }
    });
  }).observe(document.body, { childList: true, subtree: true });
}

// Swallow wasm runtime errors thrown from PPU/SPU pthread workers — until
// the Phase 4 wasm32 audit (plan-poc-ps3-webgpu.md) lands, those workers
// abort on unaligned guest-memory accesses. We don't want the CRA dev
// error overlay covering the live canvas every time that happens; the
// errors are still logged via console.error so they're recoverable for
// debugging. Returning true from window.onerror suppresses the default
// (overlay-triggering) handling.
const WASM_RUNTIME_ERROR_PATTERNS = [
  'memory access out of bounds',
  'operation does not support unaligned accesses',
  'Aborted(native code called abort())',
];
window.addEventListener('error', (ev) => {
  const msg = ev?.error?.message || ev?.message || '';
  if (WASM_RUNTIME_ERROR_PATTERNS.some(p => msg.includes(p))) {
    ev.preventDefault();
    ev.stopImmediatePropagation();
    // Log stack to see which wasm function (with --profiling-funcs)
    // triggered the crash — needed to localize the wasm32 audit work.
    const stack = ev?.error?.stack || '<no stack>';
    console.warn('[index] swallowed wasm runtime error:', msg.slice(0, 200), '\n', stack.slice(0, 1500));
    return true;
  }
  return undefined;
}, true);
window.addEventListener('unhandledrejection', (ev) => {
  const msg = ev?.reason?.message || String(ev?.reason || '');
  if (WASM_RUNTIME_ERROR_PATTERNS.some(p => msg.includes(p))) {
    ev.preventDefault();
    console.warn('[index] swallowed wasm promise rejection:', msg.slice(0, 200));
  }
});

// Compare mode: `?compare=<game-key>` (keys defined in Games.ts) — render
// the side-by-side reference image vs. live WebGPU output. Any value
// enables compare mode; unknown keys fall back to the default game in
// Games.ts.
const compareParam = new URLSearchParams(window.location.search).get('compare');
const isCompare = compareParam !== null;

ReactDOM.render(
  <React.StrictMode>
    {isCompare ? (
      <Compare game={compareParam ?? ''} />
    ) : (
      <Provider store={store}>
        <App />
      </Provider>
    )}
  </React.StrictMode>,
  document.getElementById('root')
);

reportWebVitals();
