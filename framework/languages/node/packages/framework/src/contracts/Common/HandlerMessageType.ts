import type { Type, ZLinkMessageTypeResolver } from './CoreTypes';

type HandlerTypeWithMessageType = Type & {
  readonly messageType?: ZLinkMessageTypeResolver;
};

/** Reads a handler's declared type once and defers/caches a lazy resolver until decode. */
export function resolveHandlerMessageType(handlerType: Type): ZLinkMessageTypeResolver | undefined {
  const declared = (handlerType as HandlerTypeWithMessageType).messageType;
  if (declared === undefined || isTypeConstructor(declared)) return declared;

  let state: 'unresolved' | 'resolved' | 'failed' = 'unresolved';
  let resolved: Type | undefined;
  let failure: unknown;
  return () => {
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
  };
}

/** Converts a stored registration resolver to the constructor expected by a codec. */
export function resolveMessageTypeForDecode(
  messageType?: ZLinkMessageTypeResolver
): Type | undefined {
  if (messageType === undefined) return undefined;
  return isTypeConstructor(messageType) ? messageType : messageType();
}

function isTypeConstructor(value: unknown): value is Type {
  // Class constructors have a non-writable prototype property; ordinary resolver functions do not.
  return (
    typeof value === 'function' &&
    Object.getOwnPropertyDescriptor(value, 'prototype')?.writable === false
  );
}
