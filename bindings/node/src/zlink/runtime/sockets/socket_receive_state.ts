// SPDX-License-Identifier: MPL-2.0

const bufferedReceiveSockets = new WeakSet<object>();

export function setBufferedReceive(socket: object, buffered: boolean): void {
  if (buffered) {
    bufferedReceiveSockets.add(socket);
  } else {
    bufferedReceiveSockets.delete(socket);
  }
}

export function hasBufferedReceive(socket: object): boolean {
  return bufferedReceiveSockets.has(socket);
}
