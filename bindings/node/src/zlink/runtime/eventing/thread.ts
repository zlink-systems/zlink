// SPDX-License-Identifier: MPL-2.0

import { Worker } from 'node:worker_threads';

export class Thread {
  private readonly _state = new Int32Array(new SharedArrayBuffer(4));
  private readonly _worker: Worker;
  constructor(handler: () => void) {
    if (typeof handler !== 'function') {
      throw new TypeError('handler must be a function');
    }
    const source = `
      const { workerData } = require('node:worker_threads');
      const state = new Int32Array(workerData.state);
      (async () => {
        try {
          const handler = (${handler.toString()});
          await handler();
          Atomics.store(state, 0, 1);
        } catch {
          Atomics.store(state, 0, 2);
        } finally {
          Atomics.notify(state, 0);
        }
      })();
    `;
    this._worker = new Worker(source, {
      eval: true,
      workerData: { state: this._state.buffer }
    });
    this._worker.on('error', () => {
      Atomics.store(this._state, 0, 2);
      Atomics.notify(this._state, 0);
    });
    this._worker.on('exit', (code) => {
      if (code !== 0 && Atomics.load(this._state, 0) === 0) {
        Atomics.store(this._state, 0, 2);
        Atomics.notify(this._state, 0);
      }
    });
  }
  join(): void {
    while (Atomics.load(this._state, 0) === 0) {
      Atomics.wait(this._state, 0, 0);
    }
    if (Atomics.load(this._state, 0) === 2) {
      throw new Error('thread handler failed');
    }
  }
}
