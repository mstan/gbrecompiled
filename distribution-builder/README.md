# Pokemon Distribution Builder

Static web app for editing the Pokemon distribution files the
gb-recompiled runtime injects via its Distributions menu.

- Reads `.pkm`, `.pk1`, and `.pk2` so existing distributions can be
  loaded into the form and tweaked.
- Writes `.pkm` only — the runtime-native generation-agnostic text
  format. PKHeX / EventsGallery already cover the binary side, and
  the runtime accepts `.pkm` happily, so the builder doesn't try to
  re-encode binary.

## Running locally

`index.html` is a plain static page that loads ES modules via
`<script type="module">`, so a tiny local file server is needed
(browsers refuse `file://` modules):

```sh
cd distribution-builder
python3 -m http.server 8000
# then open http://localhost:8000
```

## Deploying

Drop the folder onto any static host; for GitHub Pages, point
`Settings → Pages → Source` at the gb-recompiled repo and set the
deploy path to `/distribution-builder`.

## Data

The species and move tables are generated from the pret disassemblies
by `tools/generate_data.py`:

```sh
cd tools
python3 generate_data.py
# wrote gen1.json: 151 entries
# wrote gen2.json: 251 entries
# wrote moves.json: 251 entries
```

Sources (override via env):

- `POKERED_SRC` (default `~/Documents/Development/pokered`)
- `POKECRYSTAL_SRC` (default `~/Documents/Development/pokecrystal-decomp`)

## Files

- `index.html`, `css/styles.css` — page layout and theme.
- `js/app.js` — form ↔ model glue, file IO, download.
- `js/pkm.js` — `.pkm` text parser and serializer.
- `js/binary.js` — `.pk1`/`.pk2` binary parser (read-only).
- `js/charmap.js` — Gen 1/2 cart text decoding.
- `js/stats.js` — DV nibble unpacking.
- `js/data.js` — loads and exposes the bundled JSON.
- `data/gen1.json`, `data/gen2.json`, `data/moves.json` — species and moves.
- `tools/generate_data.py` — regenerates `data/` from pret sources.
