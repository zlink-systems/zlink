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
    slab = Buffer.allocUnsafe(SLAB_BYTES);
    offset = 0;
  }
  const bytes = slab.subarray(offset, offset + length);
  offset += Math.ceil(length / ALIGNMENT) * ALIGNMENT;
  // Transient runtime frames only; local queues and application boundaries
  // copy before they retain or expose these bytes.
  return bytes;
}

/** Copies bytes into an exact, standalone backing store with no Buffer pool. */
export function copyPayloadBytes(bytes: Uint8Array): Buffer {
  const copy = Buffer.allocUnsafeSlow(bytes.byteLength);
  copy.set(bytes);
  return copy;
}
