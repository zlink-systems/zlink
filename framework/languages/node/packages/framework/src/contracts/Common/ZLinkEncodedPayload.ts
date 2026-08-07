import { borrowEncodedPayload, storeEncodedPayload } from './encoded-payload-storage';

export class ZLinkEncodedPayload {
  private constructor(bytes: Uint8Array) {
    storeEncodedPayload(this, bytes);
  }

  static from(bytes: Uint8Array): ZLinkEncodedPayload {
    return new ZLinkEncodedPayload(bytes);
  }

  data(): Uint8Array {
    return new Uint8Array(this.stored());
  }

  toBytes(): Uint8Array {
    return this.data();
  }

  copy(): ZLinkEncodedPayload {
    return ZLinkEncodedPayload.from(this.stored());
  }

  size(): number {
    return this.stored().length;
  }

  isEmpty(): boolean {
    return this.stored().length === 0;
  }

  getString(encoding: BufferEncoding = 'utf8'): string {
    return this.stored().toString(encoding);
  }

  close(): void {
    // ZLinkEncodedPayload owns managed memory only; this mirrors Message-like readers.
  }

  private stored(): Buffer {
    const payload = borrowEncodedPayload(this);
    if (payload === undefined) {
      throw new Error('ZLink encoded payload storage is unavailable.');
    }
    return payload;
  }
}
