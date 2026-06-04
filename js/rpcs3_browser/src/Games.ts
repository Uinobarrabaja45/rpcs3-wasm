// PS3 sample titles for the compare / auto-load flow. Same shape as Play!'s
// Games.ts: `name` + `ref` drive the split-screen reference render, while
// `rom` + `state` (when present) let App.tsx auto-load a known-good ROM and
// save state from the local CI fileserver. Reference-only games (no rom yet)
// fall back to a default runnable game via getRunnableGame().

export type GameDef = {
  name: string;
  ref: string;
  rom?: string;
  state?: string;
};

const TERRARIA: Required<GameDef> = {
  name: 'Terraria',
  ref: 'terraria.png',
  rom: 'terraria.iso',
  state: 'demons_souls_state.bin',
};
const DRAGON_CROWN: Required<GameDef> = {
  name: 'Dragon Crown',
  ref: 'dragoncrown.png',
  rom: 'dragoncrown.iso',
  state: 'dragoncrown_state.bin',
};

export const GAMES: Record<string, GameDef> = {

  terraria: TERRARIA,
  dragoncrown: DRAGON_CROWN,
  uncharted: { name: 'Uncharted: Drake\'s Fortune', ref: 'HUDref_uncharted.png', rom: 'uncharted.iso', state: 'uncharted_state.bin' },
  lbp: { name: 'LittleBigPlanet', ref: 'HUDref_lbp.png', rom: 'lbp.iso', state: 'lbp_state.bin' },
  mgs4: { name: 'Metal Gear Solid 4', ref: 'HUDref_mgs4.png', rom: 'mgs4.iso', state: 'mgs4_state.bin' },
  god_of_war_3: { name: 'God of War III', ref: 'HUDref_gow3.png', rom: 'gow3.iso', state: 'gow3_state.bin' },
  the_last_of_us: { name: 'The Last of Us', ref: 'HUDref_tlou.png', rom: 'tlou.iso', state: 'tlou_state.bin' },
  ps3_xmb: { name: 'PS3 XMB (Boot)', ref: 'HUDref_xmb.png' },
};

export function getRunnableGame(key: string | null | undefined): Required<GameDef> {
  const candidate = key ? GAMES[key] : undefined;
  if (candidate?.rom) {
    return candidate as Required<GameDef>;
  }
  return DRAGON_CROWN;
}
