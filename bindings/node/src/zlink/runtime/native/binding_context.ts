// SPDX-License-Identifier: MPL-2.0

import type { NativeBuffer, NativeHandle } from './binding_types';
import type { CoreHwmBudgetSnapshot } from '../../contracts/core';

export type CoreHwmBudgetSnapshotRaw = Omit<
  CoreHwmBudgetSnapshot,
  'budgetPlanningActive' | 'budgetInsufficient' |
  'aggregateHwmValid' | 'aggregateOverflow'
>;

export interface ContextNativeBinding {
  ctxGetOpt: (ctx: NativeHandle, option: number) => number;
  ctxGetOptData: (ctx: NativeHandle, option: number) => NativeBuffer;
  ctxGetAutoHwmBudgetSnapshot: (ctx: NativeHandle) => CoreHwmBudgetSnapshotRaw;
  ctxNew: () => NativeHandle;
  ctxRecalculateAutoHwm: (ctx: NativeHandle) => void;
  ctxResetAutoHwmBudgetMetrics: (ctx: NativeHandle) => void;
  ctxSetOpt: (ctx: NativeHandle, option: number, value: number | NativeBuffer) => void;
  ctxShutdown: (ctx: NativeHandle) => void;
  ctxTerm: (ctx: NativeHandle) => void;
}
