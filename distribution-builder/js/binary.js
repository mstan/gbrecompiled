/**
 * .pk1 / .pk2 binary format - read-only.
 *
 * The builder loads these so the user can edit a binary distribution
 * and re-export it as the runtime-native .pkm. Writing binary files
 * is intentionally not supported - PKHeX / EventsGallery already do
 * that, and the runtime is happy with .pkm.
 *
 * Layout (matches PKHeX / EventsGallery):
 *   [0]  count = 1
 *   [1]  species byte (matches body[0])
 *   [2]  0xFF terminator
 *   [3..3+body_size)        body (44 PK1 / 48 PK2) - exact party_struct
 *   [3+body+0..+name_len)   OT name (charmap, $50-padded)
 *   [3+body+name_len..)     Nickname (charmap, $50-padded)
 *
 * Sizes:
 *   PK1 international: 69 bytes (11-byte names), body 44
 *   PK1 Japanese     : 59 bytes (6-byte names)
 *   PK2 international: 73 bytes
 *   PK2 Japanese     : 63 bytes
 */

import { decodeName } from './charmap.js';
import { unpackDVs } from './stats.js';

export function detectFormat(extension, byteLength) {
  const ext = extension.toLowerCase();
  if (ext === 'pk1' && (byteLength === 69 || byteLength === 59)) {
    return { gen: 1, bodySize: 44, nameLen: byteLength === 59 ? 6 : 11 };
  }
  if (ext === 'pk2' && (byteLength === 73 || byteLength === 63)) {
    return { gen: 2, bodySize: 48, nameLen: byteLength === 63 ? 6 : 11 };
  }
  return null;
}

export function parseBinary(buf, ext) {
  const fmt = detectFormat(ext, buf.length);
  if (!fmt) throw new Error(`unrecognized ${ext} size ${buf.length}`);
  const { gen, bodySize, nameLen } = fmt;
  const body = buf.slice(3, 3 + bodySize);
  const otName  = decodeName(buf.slice(3 + bodySize, 3 + bodySize + nameLen));
  const nickname = decodeName(buf.slice(3 + bodySize + nameLen,
                                       3 + bodySize + 2 * nameLen));

  const otIdOff = gen === 2 ? 6 : 12;
  const otId = (body[otIdOff] << 8) | body[otIdOff + 1];

  const movesOff = gen === 2 ? 2 : 8;
  const moves = [body[movesOff], body[movesOff + 1],
                 body[movesOff + 2], body[movesOff + 3]];

  const dvsOff = gen === 2 ? 21 : 27;
  const dvs = unpackDVs(body[dvsOff], body[dvsOff + 1]);

  const level = gen === 2 ? body[31] : body[33];
  const species = body[0];

  const model = {
    species,
    level,
    shiny: dvs.def === 10 && dvs.spd === 10 && dvs.spc === 10 &&
           (dvs.atk & 0x02) !== 0,
    nickname,
    ot_name: otName,
    ot_id: otId,
    moves,
    dvs,
    held_item: gen === 2 ? body[1]  : null,
    happiness: gen === 2 ? body[27] : null,
    pokerus:   gen === 2 ? body[28] : null,
    catch_rate: gen === 1 ? body[7] : null,
  };
  return { model, gen, nameLen };
}
