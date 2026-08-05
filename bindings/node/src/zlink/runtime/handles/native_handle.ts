// SPDX-License-Identifier: MPL-2.0

const nativeHandles = new WeakMap<NativeHandle, unknown | null>();

export function getNativeHandle(handle: NativeHandle): unknown {
  return nativeHandles.get(handle) ?? null;
}

export class NativeHandle {
  protected constructor(native: unknown) {
    nativeHandles.set(this, native);
  }

  protected get _native(): unknown | null {
    return nativeHandles.get(this) ?? null;
  }

  protected set _native(native: unknown | null) {
    nativeHandles.set(this, native);
  }

  close(): void {
    this._native = null;
  }
}
