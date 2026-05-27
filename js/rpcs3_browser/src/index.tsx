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
