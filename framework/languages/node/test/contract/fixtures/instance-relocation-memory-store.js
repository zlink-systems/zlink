class MemoryRelocationStore {
  blobs = new Map();
  async read(reference, signal) {
    signal?.throwIfAborted();
    const storeNow = new Date();
    const blob = this.blobs.get(reference.value);
    if (!blob || blob.expiresAt <= storeNow) return { kind: 'missing', storeNow };
    return { kind: 'found', ...blob, bytes: Buffer.from(blob.bytes), storeNow };
  }
  async put(reference, payload, retentionMs, signal) {
    const current = await this.read(reference, signal);
    const storeNow = new Date();
    if (current.kind === 'found')
      return Buffer.from(payload).equals(current.bytes)
        ? { kind: 'alreadyStored', expiresAt: current.expiresAt, storeNow }
        : { kind: 'conflict', storeNow };
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: Buffer.from(payload), expiresAt });
    return { kind: 'stored', expiresAt, storeNow };
  }
  async renew(reference, retentionMs, signal) {
    const current = await this.read(reference, signal);
    if (current.kind === 'missing') return current;
    const storeNow = new Date();
    const expiresAt = new Date(storeNow.getTime() + retentionMs);
    this.blobs.set(reference.value, { bytes: current.bytes, expiresAt });
    return { kind: 'renewed', expiresAt, storeNow };
  }
  async delete(reference, signal) {
    signal?.throwIfAborted();
    this.blobs.delete(reference.value);
  }
}

module.exports = { MemoryRelocationStore };
