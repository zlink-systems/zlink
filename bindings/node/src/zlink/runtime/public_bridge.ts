// SPDX-License-Identifier: MPL-2.0

import type { BaseSocket, Context } from '../contracts';
import type { RuntimeContext } from './core/context';
import { NativeHandle } from './handles/native_handle';
import type { RuntimeBaseSocket } from './sockets';

function assertRuntimeHandle(value: unknown, name: string): asserts value is NativeHandle {
  if (!(value instanceof NativeHandle)) {
    throw new TypeError(`${name} must be created by this zlink package`);
  }
}

export function asRuntimeContext(ctx: Context): RuntimeContext {
  assertRuntimeHandle(ctx, 'context');
  return ctx as RuntimeContext;
}

export function asRuntimeSocket(socket: BaseSocket): RuntimeBaseSocket {
  assertRuntimeHandle(socket, 'socket');
  return socket as unknown as RuntimeBaseSocket;
}

export function asPublicContract<T>(value: unknown): T {
  return value as T;
}
