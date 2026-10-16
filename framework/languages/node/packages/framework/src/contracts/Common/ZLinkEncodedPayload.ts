import {
  adoptEncodedPayload,
  borrowEncodedPayload,
  storeEncodedPayload
} from './encoded-payload-storage';

export class ZLinkEncodedPayload {
  private constructor(bytes: Uint8Array, owned: boolean) {
    if (owned) {
      adoptEncodedPayload(this, bytes);
    } else {
      storeEncodedPayload(this, bytes);
    }
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
    return new Uint8Array(this.stored());
  }

  /** Returns the stored bytes without copying. Do not modify this view or its backing buffer. */
  view(): Readonly<Uint8Array> {
    return this.stored();
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
