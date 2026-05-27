import type { CSSProperties } from 'react';
import { GAMES, getRunnableGame } from './Games';

// Side-by-side compare: live WebGPU emulation in an iframe vs. a static
// reference image (typically rendered with native RPCS3 Vulkan). `?compare=
// <game-key>` selects from GAMES; unknown keys fall back to demons (Demon's
// Souls), the project's hero title.

const container: CSSProperties = {
  display: 'flex',
  width: '100%',
  height: '100%',
  background: '#000',
};

const pane: CSSProperties = {
  position: 'relative',
  flex: '1 1 50%',
  minWidth: 0,
  height: '100%',
  display: 'flex',
  alignItems: 'center',
  justifyContent: 'center',
  overflow: 'hidden',
};

const label: CSSProperties = {
  position: 'absolute',
  top: 8,
  left: 8,
  zIndex: 10,
  padding: '4px 10px',
  font: '12px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif',
  color: '#fff',
  background: 'rgba(0, 0, 0, 0.65)',
  borderRadius: 4,
  pointerEvents: 'none',
};

const liveFrame: CSSProperties = {
  width: '100%',
  height: '100%',
  border: 0,
  display: 'block',
};

const referenceImg: CSSProperties = {
  width: '100%',
  height: 'auto',
  display: 'block',
};

function Compare({ game }: { game: string }) {
  // Use getRunnableGame so unknown keys (e.g. ?compare=banana) fall back
  // to the default rather than crashing on info.name / info.ref / info.rom
  // lookups. `sel` is what we forward to the live iframe via ?game=.
  const sel = GAMES[game] ? game : 'terraria';
  const info = GAMES[sel] ?? getRunnableGame(null);
  // ram=1024: the stable fixed-heap build. ram=dyna creeps up over a long
  // soak session and eventually wedges the page — the compare view is
  // exactly the soaking scenario, so use the fixed-size build.
  const urlParams = new URLSearchParams(window.location.search);
  const waitMsParam = urlParams.get('waitMs');
  // loadFixedRom=true makes App.tsx fetch the game's ROM from
  // FIXED_ROM_BASE (http://localhost:8008) and boot it automatically as
  // soon as the WASM hits 'initialized'. Same flow the standalone page
  // exposes via ?loadFixedRom=true — wiring it on by default for the
  // compare view so the left pane shows live emulation without a manual
  // file pick.
  const liveUrl =
    `${process.env.PUBLIC_URL}/?ram=1024&webgpu=true&ignore_log=false` +
    `&autoInit=true&mute=true&loadFixedRom=true&game=${sel}` +
    (waitMsParam ? `&waitMs=${waitMsParam}` : '');
  const referenceImage = `${process.env.PUBLIC_URL}/${info.ref}`;

  return (
    <div style={container}>
      <div style={pane}>
        <div style={label}>Live · WebGPU · {info.name}</div>
        <iframe
          src={liveUrl}
          title="Live emulator output"
          style={liveFrame}
          allow="autoplay; fullscreen; gamepad"
        />
      </div>
      <div style={{ ...pane, borderLeft: '2px solid #333', alignItems: 'flex-start' }}>
        <div style={label}>Reference · Vulkan ({info.ref})</div>
        <img src={referenceImage} alt="Vulkan reference" style={referenceImg} />
      </div>
    </div>
  );
}

export default Compare;
