import type { Message } from '../../contracts/Common/Message';
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
  let current = initial;
  return {
    get bytes() {
      return current;
    },
    toBytes() {
      return new Uint8Array(current);
    },
    data() {
      return current;
    },
    size() {
      return current.byteLength;
    },
    isEmpty() {
      return current.byteLength === 0;
    },
    copy() {
      return createSimpleMessage(new Uint8Array(current), true);
    },
    getString() {
      return utf8Decode(current);
    },
    value() {
      return JSON.parse(utf8Decode(current));
    },
    close() {
      if (releaseOnClose) {
        current = EMPTY_MESSAGE_BYTES;
      }
    }
  };
}
