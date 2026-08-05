// SPDX-License-Identifier: MPL-2.0

import type { NativeHandle, NativeVersion, NullableNativeHandle } from './binding_types';

export interface CoreNativeBinding {
  errno: () => number;
  has: (capability: string) => boolean;
  proxy: (
    frontend: NativeHandle,
    backend: NativeHandle,
    capture: NullableNativeHandle
  ) => void;
  proxySteerable: (
    frontend: NativeHandle,
    backend: NativeHandle,
    capture: NullableNativeHandle,
    control: NativeHandle
  ) => void;
  sleep: (seconds: number) => void;
  strerror: (code: number) => string;
  version: () => NativeVersion;
}
