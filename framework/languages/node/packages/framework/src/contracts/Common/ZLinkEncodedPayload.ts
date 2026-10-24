export class ZLinkEncodedPayload {
  readonly #bytes: Buffer;

  private constructor(bytes: Uint8Array, owned: boolean) {
    this.#bytes = !owned
      ? Buffer.from(bytes)
      : Buffer.isBuffer(bytes)
        ? bytes
        : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  /** Copies the supplied bytes. */
  static from(bytes: Uint8Array): ZLinkEncodedPayload {
    return new ZLinkEncodedPayload(bytes, false);
  }

  /** Retains the supplied bytes without copying. Do not modify them afterward; mutation changes the payload. */
  static fromOwned(bytes: Uint8Array): ZLinkEncodedPayload {
    return new ZLinkEncodedPayload(bytes, true);
  }

  /** Returns a copy of the stored bytes. */
  data(): Uint8Array {
    return new Uint8Array(this.#bytes);
  }

  /** Returns the stored bytes without copying. Do not modify this view or its backing buffer. */
  view(): Readonly<Uint8Array> {
    return this.#bytes;
  }

  toBytes(): Uint8Array {
    return this.data();
  }

  copy(): ZLinkEncodedPayload {
    return ZLinkEncodedPayload.from(this.#bytes);
  }

  size(): number {
    return this.#bytes.length;
  }

  isEmpty(): boolean {
    return this.#bytes.length === 0;
  }

  getString(encoding: BufferEncoding = 'utf8'): string {
    return this.#bytes.toString(encoding);
  }

  close(): void {
    // ZLinkEncodedPayload owns managed memory only; this mirrors Message-like readers.
  }
}
