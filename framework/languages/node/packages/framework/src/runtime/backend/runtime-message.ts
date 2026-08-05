import type { Message } from '../../contracts/Common/Message';

export class ZLinkBufferMessage implements Message {
  private readonly bytes: Buffer;

  private constructor(bytes: Buffer) {
    this.bytes = bytes;
  }

  static from(value: Message | Buffer | Uint8Array | string): ZLinkBufferMessage {
    if (typeof value === 'string') return new ZLinkBufferMessage(Buffer.from(value));
    if (isMessage(value)) return new ZLinkBufferMessage(Buffer.from(value.data()));
    return new ZLinkBufferMessage(Buffer.from(value));
  }

  static fromOwned(bytes: Buffer): ZLinkBufferMessage {
    return new ZLinkBufferMessage(bytes);
  }

  data(): Buffer {
    return this.bytes;
  }

  toBytes(): Uint8Array {
    return Buffer.from(this.bytes);
  }

  copy(): Message {
    return ZLinkBufferMessage.from(this.bytes);
  }

  size(): number {
    return this.bytes.byteLength;
  }

  isEmpty(): boolean {
    return this.bytes.byteLength === 0;
  }

  getString(encoding: BufferEncoding = 'utf8'): string {
    return this.bytes.toString(encoding);
  }

  close(): void {}
}

function isMessage(value: unknown): value is Message {
  return typeof value === 'object'
    && value !== null
    && typeof (value as { data?: unknown }).data === 'function';
}
