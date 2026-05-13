/**
 * Gen 1/2 cart text decoding.
 *
 * Only the decode path is needed - the builder reads names out of
 * loaded .pk1/.pk2 files but never writes binary, so encoding back to
 * the cart's charmap isn't required. The runtime handles that side
 * when injecting from .pkm.
 */

export const TERMINATOR = 0x50;

export function decodeName(bytes) {
  let out = '';
  for (const b of bytes) {
    if (b === TERMINATOR) break;
    if (b >= 0x80 && b <= 0x99) out += String.fromCharCode(0x41 + (b - 0x80));
    else if (b >= 0xA0 && b <= 0xB9) out += String.fromCharCode(0x61 + (b - 0xA0));
    else if (b === 0x7F) out += ' ';
    else if (b === 0xE0) out += "'";
    else if (b === 0xE8) out += '.';
    else if (b >= 0xF6 && b <= 0xFF) out += String.fromCharCode(0x30 + (b - 0xF6));
    else out += '?';
  }
  return out;
}
