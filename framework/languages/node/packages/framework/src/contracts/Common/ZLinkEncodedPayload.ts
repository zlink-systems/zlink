import { storeEncodedPayload } from './encoded-payload-storage';

export class ZLinkEncodedPayload {
  private readonly payload: Buffer;

  private constructor(bytes: Uint8Array) {
    this.payload = storeEncodedPayload(this, bytes);
  }

  static from(bytes: Uint8Array): ZLinkEncodedPayload {
    return new ZLinkEncodedPayload(bytes);
  }

  data(): Uint8Array {
    return new Uint8Array(this.payload);
  }

  toBytes(): Uint8Array {
    return this.data();
  }

  copy(): ZLinkEncodedPayload {
    return ZLinkEncodedPayload.from(this.payload);
  }

  size(): number {
    return this.payload.length;
  }

  isEmpty(): boolean {
    return this.payload.length === 0;
  }

  getString(encoding: BufferEncoding = 'utf8'): string {
    return this.payload.toString(encoding);
  }

  close(): void {
    // ZLinkEncodedPayload owns managed memory only; this mirrors Message-like readers.
  }
}
