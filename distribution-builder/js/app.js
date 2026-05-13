/**
 * Wires the form to the encoders/decoders.
 *
 * Read flow:  load file -> detect format -> parse -> populate form
 * Write flow: read form -> resolve names against data -> encode + download
 */

import { loadData, speciesForGen, movesForGen, findSpecies, findMove }
  from './data.js';
import { parsePKM, formatPKM, emptyModel as emptyPKM } from './pkm.js';
import { parseBinary } from './binary.js';

let data;
let currentModel = emptyPKM();

document.addEventListener('DOMContentLoaded', async () => {
  data = await loadData();
  initGenRadios();
  rebuildSpeciesAndMoves();
  buildItemDropdown();
  bindFormInputs();
  bindFileLoad();
  bindSaveButtons();
  syncFormFromModel();
  refreshPreview();
});

function buildItemDropdown() {
  const sel = document.getElementById('held-item');
  sel.innerHTML = '';
  // Blank default option so the field reads empty like the adjacent
  // number inputs. Selecting it puts the model back to null and the
  // .pkm omits the held_item key entirely. ID 0 (None) is a separate
  // "explicitly hold nothing" choice further down the list.
  const blank = document.createElement('option');
  blank.value = '';
  blank.textContent = '';
  sel.appendChild(blank);
  for (const it of data.items) {
    const opt = document.createElement('option');
    opt.value = String(it.id);
    opt.textContent = `${String(it.id).padStart(3, '0')} - ${it.name}`;
    sel.appendChild(opt);
  }
}

function getGen() {
  const checked = document.querySelector('input[name="gen"]:checked');
  return parseInt(checked.value, 10);
}

function setGen(gen) {
  document.body.dataset.gen = String(gen);
  document.querySelector(`input[name="gen"][value="${gen}"]`).checked = true;
}

function initGenRadios() {
  setGen(1);
  for (const r of document.querySelectorAll('input[name="gen"]')) {
    r.addEventListener('change', () => {
      document.body.dataset.gen = String(getGen());
      rebuildSpeciesAndMoves();
      syncFormFromModel();
      refreshPreview();
    });
  }
}

function rebuildSpeciesAndMoves() {
  const gen = getGen();
  const speciesSel = document.getElementById('species');
  speciesSel.innerHTML = '';
  const placeholder = document.createElement('option');
  placeholder.value = '';
  placeholder.textContent = '(choose)';
  speciesSel.appendChild(placeholder);
  for (const s of speciesForGen(data, gen)) {
    const opt = document.createElement('option');
    opt.value = String(s.dex);
    opt.textContent = `${String(s.dex).padStart(3, '0')} - ${s.name}`;
    speciesSel.appendChild(opt);
  }

  const moves = movesForGen(data, gen);
  for (let slot = 0; slot < 4; slot++) {
    const sel = document.getElementById(`move-${slot}`);
    sel.innerHTML = '';
    const empty = document.createElement('option');
    empty.value = '';
    empty.textContent = '(empty)';
    sel.appendChild(empty);
    for (const m of moves) {
      const opt = document.createElement('option');
      opt.value = String(m.id);
      opt.textContent = m.name;
      sel.appendChild(opt);
    }
  }
}

function bindFormInputs() {
  const ids = [
    'species', 'level', 'shiny', 'nickname', 'ot-name', 'ot-id',
    'move-0', 'move-1', 'move-2', 'move-3',
    'dv-atk', 'dv-def', 'dv-spd', 'dv-spc',
    'held-item', 'happiness', 'pokerus', 'catch-rate',
  ];
  for (const id of ids) {
    const el = document.getElementById(id);
    if (!el) continue;
    el.addEventListener('input', () => {
      syncModelFromForm();
      refreshPreview();
    });
  }
}

function syncFormFromModel() {
  const m = currentModel;
  const gen = getGen();

  // Species: model may carry a name or a dex#. Match against current data.
  let speciesDex = '';
  if (typeof m.species === 'number') {
    speciesDex = String(m.species);
  } else if (typeof m.species === 'string' && m.species) {
    const found = findSpecies(data, gen, m.species);
    if (found) speciesDex = String(found.dex);
  }
  document.getElementById('species').value = speciesDex;

  document.getElementById('level').value = m.level ?? '';
  document.getElementById('shiny').checked = !!m.shiny;
  document.getElementById('nickname').value = m.nickname ?? '';
  document.getElementById('ot-name').value = m.ot_name ?? '';
  document.getElementById('ot-id').value = m.ot_id ?? '';

  for (let i = 0; i < 4; i++) {
    const sel = document.getElementById(`move-${i}`);
    const raw = m.moves[i];
    if (raw == null || raw === '') { sel.value = ''; continue; }
    if (typeof raw === 'number') { sel.value = String(raw); continue; }
    const mv = findMove(data, gen, raw);
    sel.value = mv ? String(mv.id) : '';
  }

  document.getElementById('dv-atk').value = m.dvs.atk ?? '';
  document.getElementById('dv-def').value = m.dvs.def ?? '';
  document.getElementById('dv-spd').value = m.dvs.spd ?? '';
  document.getElementById('dv-spc').value = m.dvs.spc ?? '';

  document.getElementById('held-item').value  = m.held_item  ?? '';
  document.getElementById('happiness').value  = m.happiness  ?? '';
  document.getElementById('pokerus').value    = m.pokerus    ?? '';
  document.getElementById('catch-rate').value = m.catch_rate ?? '';
}

function numOrNull(v, { min, max } = {}) {
  if (v == null || v === '') return null;
  const n = Number(v);
  if (!Number.isFinite(n)) return null;
  if (min != null && n < min) return null;
  if (max != null && n > max) return null;
  return n;
}

function syncModelFromForm() {
  const m = currentModel;
  const dex = numOrNull(document.getElementById('species').value);
  m.species = dex; // store dex# as the canonical form
  m.level = numOrNull(document.getElementById('level').value);
  m.shiny = document.getElementById('shiny').checked ? true : null;
  m.nickname = document.getElementById('nickname').value || '';
  m.ot_name  = document.getElementById('ot-name').value || '';
  m.ot_id    = numOrNull(document.getElementById('ot-id').value);
  for (let i = 0; i < 4; i++) {
    m.moves[i] = numOrNull(document.getElementById(`move-${i}`).value);
  }
  m.dvs.atk = numOrNull(document.getElementById('dv-atk').value);
  m.dvs.def = numOrNull(document.getElementById('dv-def').value);
  m.dvs.spd = numOrNull(document.getElementById('dv-spd').value);
  m.dvs.spc = numOrNull(document.getElementById('dv-spc').value);
  m.held_item  = numOrNull(document.getElementById('held-item').value);
  m.happiness  = numOrNull(document.getElementById('happiness').value);
  m.pokerus    = numOrNull(document.getElementById('pokerus').value);
  m.catch_rate = numOrNull(document.getElementById('catch-rate').value,
                           { min: 1, max: 255 });
}

function refreshPreview() {
  const previewModel = { ...currentModel };
  if (typeof previewModel.species === 'number') {
    const s = findSpecies(data, getGen(), previewModel.species);
    if (s) previewModel.species = s.name;
  }
  previewModel.moves = previewModel.moves.map(m => {
    if (m == null) return null;
    if (typeof m === 'number') {
      const mv = findMove(data, getGen(), m);
      return mv ? mv.name : m;
    }
    return m;
  });
  document.getElementById('preview').textContent = formatPKM(previewModel, { gen: getGen() });
}

function bindFileLoad() {
  document.getElementById('file-input').addEventListener('change', async (ev) => {
    const file = ev.target.files[0];
    if (!file) return;
    const ext = file.name.split('.').pop().toLowerCase();
    const status = document.getElementById('load-status');
    try {
      if (ext === 'pkm') {
        const text = await file.text();
        currentModel = parsePKM(text);
        // .pkm doesn't indicate a generation; default to the current
        // selection but try to infer from a gen-only key if present.
        if (currentModel.catch_rate != null) setGen(1);
        else if (currentModel.held_item != null || currentModel.happiness != null) setGen(2);
        document.body.dataset.gen = String(getGen());
        rebuildSpeciesAndMoves();
        syncFormFromModel();
        status.textContent = `Loaded ${file.name}`;
      } else if (ext === 'pk1' || ext === 'pk2') {
        const buf = new Uint8Array(await file.arrayBuffer());
        const { model, gen } = parseBinary(buf, ext);
        currentModel = normalizeBinaryModel(model, gen);
        setGen(gen);
        document.body.dataset.gen = String(gen);
        rebuildSpeciesAndMoves();
        syncFormFromModel();
        status.textContent = `Loaded ${file.name} (${ext.toUpperCase()})`;
      } else {
        status.textContent = `Unsupported extension: .${ext}`;
      }
      refreshPreview();
    } catch (err) {
      status.textContent = `Failed: ${err.message}`;
    }
  });
}

function normalizeBinaryModel(model, gen) {
  // The binary parser returns the species byte; for gen 1 that's the
  // internal_id, for gen 2 it's the dex#. Convert to dex# so the
  // form select can match it.
  const out = { ...model, moves: [...model.moves], dvs: { ...model.dvs } };
  if (gen === 1) {
    const s = data[1].species.find(s => s.internal_id === model.species);
    out.species = s ? s.dex : null;
  } else {
    out.species = typeof model.species === 'number' ? model.species : null;
  }
  return out;
}

function bindSaveButtons() {
  document.getElementById('save-pkm').addEventListener('click', () => {
    const model = previewModelForExport();
    const text = formatPKM(model, { gen: getGen() });
    download(`${suggestedName()}.pkm`, new Blob([text], { type: 'text/plain' }));
    document.getElementById('save-status').textContent = 'Saved .pkm';
  });
}

function previewModelForExport() {
  // .pkm export uses names (more readable). Resolve numeric IDs back
  // to names against the active gen's tables.
  const gen = getGen();
  const m = { ...currentModel, moves: [...currentModel.moves], dvs: { ...currentModel.dvs } };
  if (typeof m.species === 'number') {
    const s = findSpecies(data, gen, m.species);
    if (s) m.species = s.name;
  }
  m.moves = m.moves.map(mv => {
    if (mv == null) return null;
    if (typeof mv !== 'number') return mv;
    const found = findMove(data, gen, mv);
    return found ? found.name : mv;
  });
  return m;
}

function suggestedName() {
  const gen = getGen();
  if (typeof currentModel.species === 'number') {
    const s = findSpecies(data, gen, currentModel.species);
    if (s) {
      const lvl = currentModel.level ?? 'lvl';
      return `${s.name.toLowerCase()}_lvl${lvl}`;
    }
  }
  return 'distribution';
}

function download(filename, blob) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}
