// SPDX-License-Identifier: MPL-2.0

export function withRuntimeErrorMessage<T extends Error>(error: T, message?: string): T {
  if (!message) {
    return error;
  }
  Object.defineProperty(error, 'message', {
    value: message,
    configurable: true,
    writable: true
  });
  return error;
}
