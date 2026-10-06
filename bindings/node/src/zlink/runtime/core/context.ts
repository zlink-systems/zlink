// SPDX-License-Identifier: MPL-2.0

import type {
  AutoHwmProfileValue,
  CoreHwmBudgetSnapshot
} from '../../contracts/core';
import { createError } from '../errors/error_mapping';
import {
  closeCall,
  configCall,
  failureErrno,
  failureResult,
  nativeErrorMessage,
} from '../errors/native_errors';
import { validateCString } from '../options/validation';
import { requireNative } from '../native/native';
import { getNativeHandle, NativeHandle } from '../handles/native_handle';
import { ContextOption } from './context_options';
import { uint64Buffer } from '../options/byte_values';

const OPTION_CREATE_TOKEN = Symbol('OptionFacade.create');

export class ContextOptions {
  /** @internal */
  protected readonly _context: ContextBase;

  /** @internal */
  private constructor(token: symbol, context: ContextBase) {
    if (token !== OPTION_CREATE_TOKEN) {
      throw new TypeError('context options are created by contexts');
    }
    this._context = context;
  }

  /** @internal */
  static create(context: ContextBase): ContextOptions {
    return new ContextOptions(OPTION_CREATE_TOKEN, context);
  }

  get ioThreads(): number { return getContextOptionRaw(this._context, ContextOption.IO_THREADS); }
  set ioThreads(value: number) { setContextOptionRaw(this._context, ContextOption.IO_THREADS, value | 0); }
  get maxSockets(): number { return getContextOptionRaw(this._context, ContextOption.MAX_SOCKETS); }
  set maxSockets(value: number) { setContextOptionRaw(this._context, ContextOption.MAX_SOCKETS, value | 0); }
  get socketLimit(): number { return getContextOptionRaw(this._context, ContextOption.SOCKET_LIMIT); }
  get msgTSize(): number { return getContextOptionRaw(this._context, ContextOption.MSG_T_SIZE); }
  get threadPriority(): number { return getContextOptionRaw(this._context, ContextOption.THREAD_PRIORITY); }
  set threadPriority(value: number) { setContextOptionRaw(this._context, ContextOption.THREAD_PRIORITY, value | 0); }
  get threadSchedulingPolicy(): number { return getContextOptionRawStrict(this._context, ContextOption.THREAD_SCHED_POLICY); }
  set threadSchedulingPolicy(value: number) { setContextOptionRaw(this._context, ContextOption.THREAD_SCHED_POLICY, value | 0); }
  get blocky(): boolean { return getContextOptionRaw(this._context, ContextOption.BLOCKY) !== 0; }
  set blocky(value: boolean) { setContextOptionRaw(this._context, ContextOption.BLOCKY, value ? 1 : 0); }
  get autoHwmEnabled(): boolean { return getContextOptionRaw(this._context, ContextOption.AUTO_HWM_ENABLE) !== 0; }
  set autoHwmEnabled(value: boolean) { setContextOptionRaw(this._context, ContextOption.AUTO_HWM_ENABLE, value ? 1 : 0); }
  get autoHwmRecalcDebounceMs(): number { return getContextOptionRaw(this._context, ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS); }
  set autoHwmRecalcDebounceMs(value: number) { setContextOptionRaw(this._context, ContextOption.AUTO_HWM_RECALC_DEBOUNCE_MS, value | 0); }
  get coreHwmProfile(): AutoHwmProfileValue { return getContextOptionRaw(this._context, ContextOption.AUTO_HWM_PROFILE) as AutoHwmProfileValue; }
  set coreHwmProfile(value: AutoHwmProfileValue) { setContextOptionRaw(this._context, ContextOption.AUTO_HWM_PROFILE, value | 0); }
  get coreHwmMemoryLimitBytes(): bigint { return getContextUInt64(this._context, ContextOption.AUTO_HWM_MEMORY_LIMIT_BYTES, 'coreHwmMemoryLimitBytes'); }
  set coreHwmMemoryLimitBytes(value: bigint) { setContextUInt64(this._context, ContextOption.AUTO_HWM_MEMORY_LIMIT_BYTES, value, 'coreHwmMemoryLimitBytes'); }
  get coreHwmBudgetBytes(): bigint { return getContextUInt64(this._context, ContextOption.AUTO_HWM_CORE_BUDGET_BYTES, 'coreHwmBudgetBytes'); }
  set coreHwmBudgetBytes(value: bigint) { setContextUInt64(this._context, ContextOption.AUTO_HWM_CORE_BUDGET_BYTES, value, 'coreHwmBudgetBytes'); }
  get threadNamePrefix(): string {
    const value = configCall('context option get failed', () =>
      requireNative().ctxGetOptData(getNativeHandle(this._context), ContextOption.THREAD_NAME_PREFIX) as Buffer);
    const end = value.indexOf(0);
    return value.subarray(0, end < 0 ? value.length : end).toString();
  }
  set threadNamePrefix(value: string) {
    const normalized = validateCString(value, 'threadNamePrefix', 15);
    setContextOptionRaw(this._context, ContextOption.THREAD_NAME_PREFIX,
      Buffer.from(`${normalized}\0`));
  }
  addThreadAffinity(cpu: number): void { setContextOptionRaw(this._context, ContextOption.THREAD_AFFINITY_CPU_ADD, cpu | 0); }
  removeThreadAffinity(cpu: number): void { setContextOptionRaw(this._context, ContextOption.THREAD_AFFINITY_CPU_REMOVE, cpu | 0); }
}

function setContextOptionRaw(context: ContextBase, option: number, value: Buffer | number): void {
  configCall('context option set failed', () => {
    requireNative().ctxSetOpt(getNativeHandle(context), option | 0, typeof value === 'number' ? value | 0 : value);
  });
}

function getContextOptionRaw(context: ContextBase, option: number): number {
  try {
    return requireNative().ctxGetOpt(getNativeHandle(context), option | 0);
  } catch (error) {
    if (
      (option | 0) === ContextOption.THREAD_PRIORITY ||
      (option | 0) === ContextOption.THREAD_SCHED_POLICY
    ) {
      return -1;
    }
    throw createError('config', failureErrno(error), nativeErrorMessage(error, 'context option get failed'), failureResult(error));
  }
}

function getContextOptionRawStrict(context: ContextBase, option: number): number {
  try {
    return requireNative().ctxGetOpt(getNativeHandle(context), option | 0);
  } catch (error) {
    const message = error instanceof Error && error.message
      ? error.message
      : 'ctx_getopt failed';
    throw createError('config', failureErrno(error), message, failureResult(error));
  }
}

function getContextUInt64(context: ContextBase, option: number, name: string): bigint {
  const value = configCall('context option get failed', () =>
    requireNative().ctxGetOptData(getNativeHandle(context), option | 0) as Buffer);
  if (value.length !== 8) throw new Error(`${name} option returned an invalid payload`);
  return value.readBigUInt64LE(0);
}

function setContextUInt64(context: ContextBase, option: number, value: bigint, name: string): void {
  setContextOptionRaw(context, option, uint64Buffer(value, name));
}

export class ContextBase extends NativeHandle {
  readonly options: ContextOptions;

  protected constructor(native: unknown) {
    super(native);
    this.options = ContextOptions.create(this);
  }

  shutdown(): void {
    closeCall('context shutdown failed', () => {
      requireNative().ctxShutdown(this._native);
    });
  }

  recalculateAutoHwm(): void {
    configCall('context auto HWM recalculation failed', () => {
      requireNative().ctxRecalculateAutoHwm(this._native);
    });
  }

  getCoreHwmBudgetSnapshot(): CoreHwmBudgetSnapshot {
    const snapshot = configCall('context HWM budget snapshot failed', () =>
      requireNative().ctxGetAutoHwmBudgetSnapshot(this._native));
    const flags = snapshot.flags >>> 0;
    return Object.freeze({
      ...snapshot,
      reservedUInt64: Object.freeze([...snapshot.reservedUInt64]),
      budgetPlanningActive: (flags & (1 << 0)) !== 0,
      budgetInsufficient: (flags & (1 << 1)) !== 0,
      aggregateHwmValid: (flags & (1 << 2)) !== 0,
      aggregateOverflow: (flags & (1 << 3)) !== 0
    });
  }

  resetCoreHwmBudgetMetrics(): void {
    configCall('context auto HWM metric reset failed', () => {
      requireNative().ctxResetAutoHwmBudgetMetrics(this._native);
    });
  }
}

export class Context extends ContextBase {
  constructor() {
    super(configCall('context creation failed', () => requireNative().ctxNew()));
    const heapLimitBytes = BigInt(Math.trunc(getHeapStatistics().heap_size_limit));
    if (heapLimitBytes > 0n) {
      setContextUInt64(this, ContextOption.AUTO_HWM_RUNTIME_MEMORY_LIMIT_BYTES,
        heapLimitBytes, 'runtimeMemoryLimitBytes');
    }
  }

  shutdown(): void {
    closeCall('context shutdown failed', () => requireNative().ctxShutdown(this._native));
  }

  close(): void {
    if (!this._native) return;
    closeCall('context shutdown failed', () => requireNative().ctxShutdown(this._native));
    closeCall('context close failed', () => requireNative().ctxTerm(this._native));
    this._native = null;
  }
}

export class SharedContext extends ContextBase {
  constructor() {
    super(configCall('shared context creation failed', () => requireNative().ctxShared()));
  }
}

export {
  ContextBase as RuntimeContext,
  ContextOptions as RuntimeContextOptions,
};
