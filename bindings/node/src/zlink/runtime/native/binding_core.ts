// SPDX-License-Identifier: MPL-2.0

import type { NativeHandle, NativeVersion, NullableNativeHandle } from './binding_types';

export interface CoreNativeBinding {
  errno: () => number;
  messageAllocate: (size: number) => { data?: Buffer; nativeMessage: unknown };
  messageFrameData: (nativeMessage: unknown) => Buffer;
  messageFrameCopyData: (nativeMessage: unknown) => Buffer;
  messageFrameCopy: (nativeMessage: unknown) => { data?: Buffer; nativeMessage: unknown };
  messageFrameMove: (
    destination: unknown,
    source: unknown,
    destinationData?: Buffer,
    sourceData?: Buffer
  ) => void;
  messageFrameRefCount: (nativeMessage: unknown) => number;
  messageFrameSize: (nativeMessage: unknown) => number;
  messageFrameClose: (nativeMessage: unknown, data?: Buffer) => void;
  messageFromBuffer: (data: Buffer) => { data?: Buffer; nativeMessage: unknown };
  has: (capability: string) => boolean;
  proxy: (
    frontend: NativeHandle,
    backend: NativeHandle,
    capture: NullableNativeHandle
  ) => void;
  sleep: (seconds: number) => void;
  strerror: (code: number) => string;
  version: () => NativeVersion;
}
