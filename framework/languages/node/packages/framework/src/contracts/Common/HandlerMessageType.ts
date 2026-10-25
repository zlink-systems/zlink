import type { Type, ZLinkMessageTypeResolver } from './CoreTypes';

type HandlerTypeWithMessageType = Type & {
  readonly messageType?: ZLinkMessageTypeResolver;
};

export interface ZLinkHandlerMessageType {
  readonly type: Type;
}

/** Reads a handler's declared type once and defers/caches a lazy resolver until decode. */
export function resolveHandlerMessageType(handlerType: Type): ZLinkHandlerMessageType | undefined {
  const declared = (handlerType as HandlerTypeWithMessageType).messageType;
  if (declared === undefined) return undefined;
  if (isTypeConstructor(declared)) return { type: declared };

  let state: 'unresolved' | 'resolved' | 'failed' = 'unresolved';
  let resolved: Type | undefined;
  let failure: unknown;
  return {
    get type() {
      if (state === 'unresolved') {
        try {
          resolved = declared();
          state = 'resolved';
        } catch (error) {
          failure = error;
          state = 'failed';
        }
      }
      if (state === 'failed') throw failure;
      return resolved!;
    }
  };
}

/** Converts a stored registration resolver to the constructor expected by a codec. */
export function resolveMessageTypeForDecode(
  messageType?: ZLinkHandlerMessageType
): Type | undefined {
  return messageType?.type;
}

function isTypeConstructor(value: unknown): value is Type {
  // Class constructors have a non-writable prototype property; ordinary resolver functions do not.
  return (
    typeof value === 'function' &&
    Object.getOwnPropertyDescriptor(value, 'prototype')?.writable === false
  );
}
