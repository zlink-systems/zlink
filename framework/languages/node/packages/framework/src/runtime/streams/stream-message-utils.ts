import type { Message } from '../../contracts/Common/Message';
import { parseFrameworkJsonV1 } from '../messaging/framework-json-v1';
import { utf8Decode, utf8Encode } from './protocol';

const EMPTY_MESSAGE_BYTES = new Uint8Array(0);

export function copyMessage(message: Message): Message {
  const value = message as unknown as {
    copy?: () => Message;
    toBytes?: () => Uint8Array;
    data?: () => Uint8Array;
    bytes?: Uint8Array;
    getString?: () => string;
  };
  if (value.copy !== undefined) {
    return value.copy();
  }
  if (value.toBytes !== undefined) {
    return simpleMessage(value.toBytes()) as Message;
  }
  if (value.data !== undefined) {
    return simpleMessage(value.data()) as Message;
  }
  if (value.bytes !== undefined) {
    return simpleMessage(value.bytes) as Message;
  }
  if (value.getString !== undefined) {
    return simpleMessage(utf8Encode(value.getString())) as Message;
  }
  throw new Error('Stream response payload cannot be copied.');
}

export function simpleMessage(bytes: Uint8Array): unknown {
  return createSimpleMessage(new Uint8Array(bytes), false);
}

/** Create the structural Message used by the internal receive assembler. */
export function ownedMessage(bytes: Uint8Array): Message {
  return createSimpleMessage(bytes, true) as Message;
}

function createSimpleMessage(initial: Uint8Array, releaseOnClose: boolean): unknown {
  return new ZLinkSimpleMessage(initial, releaseOnClose);
}

class ZLinkSimpleMessage {
  constructor(
    private current: Uint8Array,
    private readonly releaseOnClose: boolean
  ) {}

  get bytes(): Uint8Array {
    return this.current;
  }

  toBytes(): Uint8Array {
    return new Uint8Array(this.current);
  }

  data(): Uint8Array {
    return this.current;
  }

  size(): number {
    return this.current.byteLength;
  }

  isEmpty(): boolean {
    return this.current.byteLength === 0;
  }

  copy(): unknown {
    return createSimpleMessage(new Uint8Array(this.current), true);
  }

  getString(): string {
    return utf8Decode(this.current);
  }

  value(): unknown {
    return parseFrameworkJsonV1(utf8Decode(this.current));
  }

  close(): void {
    if (this.releaseOnClose) {
      this.current = EMPTY_MESSAGE_BYTES;
    }
  }
}
