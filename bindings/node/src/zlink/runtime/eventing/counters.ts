// SPDX-License-Identifier: MPL-2.0

import { requireNative } from '../native/native';
import { NativeHandle } from '../handles/native_handle';

export class Stopwatch extends NativeHandle {
  constructor() { super(requireNative().stopwatchStart()); }
  intermediate(): number { return requireNative().stopwatchIntermediate(this._native) as number; }
  stop(): number { return requireNative().stopwatchStop(this._native) as number; }
  close(): void { this._native = null; }
}

export class AtomicCounter {
  private _native: unknown | null;

  constructor(initialValue = 0) {
    this._native = requireNative().atomicCounterNew();
    if ((initialValue | 0) !== 0) {
      this.set(initialValue);
    }
  }

  set(value: number): void { requireNative().atomicCounterSet(this._native, value | 0); }
  inc(): number { return requireNative().atomicCounterInc(this._native) as number; }
  dec(): number { return requireNative().atomicCounterDec(this._native) as number; }
  value(): number { return requireNative().atomicCounterValue(this._native) as number; }
  close(): void {
    if (this._native) {
      requireNative().atomicCounterDestroy(this._native);
      this._native = null;
    }
  }
}
