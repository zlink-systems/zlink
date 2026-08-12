// SPDX-License-Identifier: MPL-2.0

import type { NativeHandle, NativeVersion, NullableNativeHandle } from './binding_types';

export interface CoreNativeBinding {
  errno: () => number;
  messageAllocate: (size: number) => { data?: Buffer; nativeMessage: unknown };
  messageFrameData: (nativeMessage: unknown) => Buffer;
  messageFrameCopyData: (nativeMessage: unknown) => Buffer;
  messageFrameSize: (nativeMessage: unknown) => number;
  messageFrameClose: (nativeMessage: unknown) => void;
  messageFromBuffer: (data: Buffer) => { data?: Buffer; nativeMessage: unknown };
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
