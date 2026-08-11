// SPDX-License-Identifier: MPL-2.0

import { setBufferedReceive } from './socket_receive_state';

/** Keeps prefetched receive values and poll readiness in one lifecycle. */
export class BufferedReceiveQueue<T> {
  private values: T[] = [];
  private index = 0;

  constructor(private readonly owner: object) {}

  take(): T | null {
    if (this.index >= this.values.length) return null;
    const value = this.values[this.index++];
    if (this.index >= this.values.length) {
      this.values = [];
      this.index = 0;
      setBufferedReceive(this.owner, false);
    }
    return value;
  }

  replace(values: T[]): void {
    this.values = values;
    this.index = 0;
    setBufferedReceive(this.owner, values.length > 0);
  }

  drain(release: (value: T) => void): void {
    for (; this.index < this.values.length; this.index += 1) {
      release(this.values[this.index]);
    }
    this.values = [];
    this.index = 0;
    setBufferedReceive(this.owner, false);
  }
}
