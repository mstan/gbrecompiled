/**
 * DV bit packing.
 *
 * Gen 1/2 stores DVs as two packed bytes — atk|def in the high byte,
 * spd|spc in the low byte (each nibble in 0-15). The .pk1/.pk2 reader
 * uses unpackDVs to surface them as an object for the form.
 */

export function unpackDVs(hi, lo) {
  return {
    atk: (hi >> 4) & 0xF,
    def: hi & 0xF,
    spd: (lo >> 4) & 0xF,
    spc: lo & 0xF,
  };
}
