// SPDX-License-Identifier: MPL-2.0

import type { Message } from '../../contracts';
import { validateCString } from '../options/validation';
import { configCall } from '../errors/native_errors';
import { getNativeHandle } from '../handles/native_handle';
import { requireNative } from '../native/native';
import type { RuntimeBaseSocket as BaseSocket } from '../sockets';

export function version(): [number, number, number] {
  return requireNative().version();
}

export function strerror(code: number): string {
  return requireNative().strerror(code);
}

export function has(capability: string): boolean {
  const normalized = validateCString(capability, 'capability', Number.MAX_SAFE_INTEGER);
  return requireNative().has(normalized);
}

export function proxy(frontend: BaseSocket, backend: BaseSocket, capture?: BaseSocket): void {
  if (capture === null) {
    throw new TypeError('capture must be a socket or undefined');
  }
  configCall('proxy failed', () => {
    requireNative().proxy(
      getNativeHandle(frontend),
      getNativeHandle(backend),
      capture ? getNativeHandle(capture) : null
    );
  });
}

export function proxySteerable(
  frontend: BaseSocket,
  backend: BaseSocket,
  capture: BaseSocket | null,
  control: BaseSocket
): void {
  configCall('proxySteerable failed', () => {
    requireNative().proxySteerable(
      getNativeHandle(frontend),
      getNativeHandle(backend),
      capture ? getNativeHandle(capture) : null,
      getNativeHandle(control)
    );
  });
}

export function sleep(seconds: number): void {
  requireNative().sleep(seconds | 0);
}

export function multipartClose(parts: Message[]): void {
  for (const part of parts) {
    part.close();
  }
}
