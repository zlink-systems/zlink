// SPDX-License-Identifier: MPL-2.0

import type { NativeBuffer, NativeHandle } from './binding_types';

export interface ContextNativeBinding {
  ctxGetOpt: (ctx: NativeHandle, option: number) => number;
  ctxGetOptData: (ctx: NativeHandle, option: number) => NativeBuffer;
  ctxNew: () => NativeHandle;
  ctxRecalculateAutoHwm: (ctx: NativeHandle) => void;
  ctxSetOpt: (ctx: NativeHandle, option: number, value: number | NativeBuffer) => void;
  ctxShutdown: (ctx: NativeHandle) => void;
  ctxTerm: (ctx: NativeHandle) => void;
}
