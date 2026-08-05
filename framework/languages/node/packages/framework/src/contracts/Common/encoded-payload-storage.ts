const encodedPayloadStorage = new WeakMap<object, Buffer>();

export function storeEncodedPayload(owner: object, bytes: Uint8Array): Buffer {
  const stored = Buffer.from(bytes);
  encodedPayloadStorage.set(owner, stored);
  return stored;
}

export function borrowEncodedPayload(owner: object): Buffer | undefined {
  return encodedPayloadStorage.get(owner);
}
