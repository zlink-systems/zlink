const SLAB_BYTES = 64 * 1024;
const MAX_POOLED_BYTES = 16 * 1024;
const ALIGNMENT = 8;

let slab: Buffer | undefined;
let offset = 0;

/** Allocates independent bytes; previously returned regions are never reused. */
export function allocatePayloadBytes(length: number): Buffer {
  if (length > MAX_POOLED_BYTES || length < 0 || !Number.isInteger(length)) {
    return Buffer.allocUnsafe(length);
  }
  if (slab === undefined || offset + length > SLAB_BYTES) {
    // Zero filled: a returned view's backing store is reachable through `.buffer`.
    slab = Buffer.alloc(SLAB_BYTES);
    offset = 0;
  }
  const bytes = slab.subarray(offset, offset + length);
  offset += Math.ceil(length / ALIGNMENT) * ALIGNMENT;
  // A retained data() view can retain its <=64 KiB slab until collection,
  // matching the retention model of Node's Buffer pool.
  return bytes;
}
