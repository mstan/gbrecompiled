/**
 * .pkm text format (generation-agnostic).
 *
 * Mirrors runtime/src/pokemon/mock_inject_file.c. All keys are
 * optional except `species` and `level`. The runtime silently drops
 * keys that don't apply to the active cart's generation.
 */

const KNOWN_KEYS = [
  'species', 'level', 'shiny', 'nickname',
  'ot_name', 'ot_id',
  'moves', 'dvs',
  'held_item', 'happiness', 'pokerus',  // gen 2
  'catch_rate',                          // gen 1
];

function emptyModel() {
  return {
    species: null,        // string (name) or number (dex#)
    level: null,
    shiny: null,          // true | false | null
    nickname: '',
    ot_name: '',
    ot_id: null,
    moves: [null, null, null, null],   // entries are string (name) or number (id)
    dvs: { atk: null, def: null, spd: null, spc: null },
    held_item: null,
    happiness: null,
    pokerus: null,
    catch_rate: null,
  };
}

export function parsePKM(text) {
  const model = emptyModel();
  for (const rawLine of text.split(/\r?\n/)) {
    let line = rawLine;
    const hash = line.indexOf('#');
    if (hash >= 0) line = line.slice(0, hash);
    line = line.trim();
    if (!line) continue;
    const eq = line.indexOf('=');
    if (eq < 0) continue;
    const key = line.slice(0, eq).trim().toLowerCase();
    const val = line.slice(eq + 1).trim();
    if (!KNOWN_KEYS.includes(key)) continue;
    applyKey(model, key, val);
  }
  return model;
}

function applyKey(model, key, val) {
  switch (key) {
    case 'species':
      model.species = /^\d+$/.test(val) ? parseInt(val, 10) : val;
      break;
    case 'level':       model.level = parseIntSafe(val, 2, 100); break;
    case 'shiny':       model.shiny = parseBool(val); break;
    case 'nickname':    model.nickname = val.slice(0, 10); break;
    case 'ot_name':     model.ot_name  = val.slice(0, 7); break;
    case 'ot_id':       model.ot_id    = parseIntSafe(val, 0, 0xFFFF); break;
    case 'moves':
      model.moves = val.split(',').slice(0, 4).map(t => {
        const s = t.trim();
        if (!s) return null;
        return /^\d+$/.test(s) ? parseInt(s, 10) : s;
      });
      while (model.moves.length < 4) model.moves.push(null);
      break;
    case 'dvs': {
      // Positional: index 0 -> atk, 1 -> def, 2 -> spd, 3 -> spc.
      // Empty position means "let the runtime roll random for this slot".
      const parts = val.split(',').map(t => {
        const s = t.trim();
        return s === '' ? null : parseIntSafe(s, 0, 15);
      });
      [model.dvs.atk, model.dvs.def, model.dvs.spd, model.dvs.spc] =
        [parts[0] ?? null, parts[1] ?? null, parts[2] ?? null, parts[3] ?? null];
      break;
    }
    case 'held_item':   model.held_item  = parseIntSafe(val, 0, 255); break;
    case 'happiness':   model.happiness  = parseIntSafe(val, 0, 255); break;
    case 'pokerus':     model.pokerus    = parseIntSafe(val, 0, 255); break;
    case 'catch_rate':  model.catch_rate = parseIntSafe(val, 1, 255); break;
  }
}

function parseIntSafe(v, lo, hi) {
  const n = parseInt(v, 10);
  if (!Number.isFinite(n) || n < lo || n > hi) return null;
  return n;
}

function parseBool(v) {
  const s = v.toLowerCase();
  if (s === 'true' || s === 'yes' || s === '1') return true;
  if (s === 'false' || s === 'no' || s === '0') return false;
  return null;
}

export function formatPKM(model, { gen } = { gen: 2 }) {
  const out = [];
  const push = (key, val) => out.push(`${key} = ${val}`);
  if (model.species != null && model.species !== '') push('species', model.species);
  if (model.level != null) push('level', model.level);
  if (model.shiny != null) push('shiny', model.shiny ? 'true' : 'false');
  if (model.nickname) push('nickname', model.nickname);
  if (model.ot_name) push('ot_name', model.ot_name);
  if (model.ot_id != null) push('ot_id', model.ot_id);

  const moveTokens = model.moves
    .map(m => m == null || m === '' ? '' : String(m));
  // Trim trailing empties so we never emit "PSYCHIC,,,"
  while (moveTokens.length && !moveTokens[moveTokens.length - 1]) moveTokens.pop();
  if (moveTokens.length) push('moves', moveTokens.join(','));

  // Partial DVs are valid: an empty position tells the runtime to
  // leave the builder's random roll in place for that slot. Only emit
  // the line if at least one slot is set; positions are comma-aligned
  // so atk/def/spd/spc stay positional.
  const dvSlots = [model.dvs.atk, model.dvs.def,
                   model.dvs.spd, model.dvs.spc];
  if (dvSlots.some(v => v != null)) {
    push('dvs', dvSlots.map(v => v == null ? '' : String(v)).join(','));
  }

  if (gen === 2) {
    if (model.held_item != null) push('held_item', model.held_item);
    if (model.happiness != null) push('happiness', model.happiness);
    if (model.pokerus   != null) push('pokerus',   model.pokerus);
  } else {
    if (model.catch_rate != null) push('catch_rate', model.catch_rate);
  }

  return out.join('\n') + '\n';
}

export { emptyModel };
