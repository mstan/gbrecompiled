/**
 * Loads the bundled JSON data files and exposes accessors keyed by
 * generation. Data lives in ../data/{gen1,gen2,moves}.json.
 */

let cache = null;

export async function loadData() {
  if (cache) return cache;
  const [g1, g2, mv, it] = await Promise.all([
    fetch(new URL('../data/gen1.json', import.meta.url)).then(r => r.json()),
    fetch(new URL('../data/gen2.json', import.meta.url)).then(r => r.json()),
    fetch(new URL('../data/moves.json', import.meta.url)).then(r => r.json()),
    fetch(new URL('../data/items.json', import.meta.url)).then(r => r.json()),
  ]);
  cache = {
    1: { species: g1.species },
    2: { species: g2.species },
    moves: mv.moves,
    items: it.items,
  };
  return cache;
}

export function speciesForGen(data, gen) { return data[gen].species; }

export function findSpecies(data, gen, identifier) {
  if (identifier == null) return null;
  if (typeof identifier === 'number') {
    return data[gen].species.find(s => s.dex === identifier) ?? null;
  }
  const want = String(identifier).toLowerCase();
  return data[gen].species.find(s => s.name.toLowerCase() === want) ?? null;
}

export function movesForGen(data, gen) {
  return data.moves.filter(m => m.gen <= gen);
}

export function findMove(data, gen, identifier) {
  if (identifier == null || identifier === '') return null;
  if (typeof identifier === 'number') {
    return data.moves.find(m => m.id === identifier && m.gen <= gen) ?? null;
  }
  const want = String(identifier).toLowerCase();
  return data.moves.find(m => m.name.toLowerCase() === want && m.gen <= gen) ?? null;
}

