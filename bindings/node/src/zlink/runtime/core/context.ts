// SPDX-License-Identifier: MPL-2.0

import type { AutoHwmProfileValue } from '../../contracts/core';
import { createError } from '../errors/error_mapping';
import {
  closeCall,
  configCall,
  lastError,
  nativeErrorMessage,
  readErrno,
} from '../errors/native_errors';
import { validateCString } from '../options/validation';
import { requireNative } from '../native/native';
import { getNativeHandle, NativeHandle } from '../handles/native_handle';
import { ContextOption } from './context_options';
import { uint64Buffer } from '../options/byte_values';

const OPTION_CREATE_TOKEN = Symbol('OptionFacade.create');

export class ContextOptions {
  /** @internal */
  protected readonly _context: Context;
  private _threadNamePrefix = '';

  /** @internal */
  private constructor(token: symbol, context: Context) {
    if (token !== OPTION_CREATE_TOKEN) {
      throw new TypeError('context options are created by contexts');
    }
    this._context = context;
  }

  /** @internal */
  static create(context: Context): ContextOptions {
    return new ContextOptions(OPTION_CREATE_TOKEN, context);
  }

  get ioThreads(): number { return getContextOptionRaw(this._context, ContextOption.IO_THREADS); }
  set ioThreads(value: number) { setContextOptionRaw(this._context, ContextOption.IO_THREADS, value | 0); }
  get maxSockets(): number { return getContextOptionRaw(this._context, ContextOption.MAX_SOCKETS); }
  set maxSockets(value: number) { setContextOptionRaw(this._context, ContextOption.MAX_SOCKETS, value | 0); }
  get socketLimit(): number { return getContextOptionRaw(this._context, ContextOption.SOCKET_LIMIT); }
  get maxMsgSize(): number { return getContextOptionRaw(this._context, ContextOption.MAX_MSGSZ); }
  set maxMsgSize(value: number) { setContextOptionRaw(this._context, ContextOption.MAX_MSGSZ, value | 0); }
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
  get autoHwmProfile(): AutoHwmProfileValue { return getContextOptionRaw(this._context, ContextOption.AUTO_HWM_PROFILE) as AutoHwmProfileValue; }
  set autoHwmProfile(value: AutoHwmProfileValue) { setContextOptionRaw(this._context, ContextOption.AUTO_HWM_PROFILE, value | 0); }
  get autoHwmMsgUnitBytes(): bigint {
    const value = requireNative().ctxGetOptData(
      getNativeHandle(this._context),
      ContextOption.AUTO_HWM_MSG_UNIT_BYTES
    );
    if (value.length !== 8) throw new Error('autoHwmMsgUnitBytes option returned an invalid payload');
    return value.readBigUInt64LE(0);
  }
  set autoHwmMsgUnitBytes(value: bigint) {
    setContextOptionRaw(
      this._context,
      ContextOption.AUTO_HWM_MSG_UNIT_BYTES,
      uint64Buffer(value, 'autoHwmMsgUnitBytes')
    );
  }
  get threadNamePrefix(): string { return this._threadNamePrefix; }
  set threadNamePrefix(value: string) {
    const normalized = validateCString(value, 'threadNamePrefix');
    setContextOptionRaw(this._context, ContextOption.THREAD_NAME_PREFIX, Buffer.from(normalized));
    this._threadNamePrefix = normalized;
  }
  addThreadAffinity(cpu: number): void { setContextOptionRaw(this._context, ContextOption.THREAD_AFFINITY_CPU_ADD, cpu | 0); }
  removeThreadAffinity(cpu: number): void { setContextOptionRaw(this._context, ContextOption.THREAD_AFFINITY_CPU_REMOVE, cpu | 0); }
}

function setContextOptionRaw(context: Context, option: number, value: Buffer | number): void {
  configCall('context option set failed', () => {
    requireNative().ctxSetOpt(getNativeHandle(context), option | 0, typeof value === 'number' ? value | 0 : value);
  });
}

function getContextOptionRaw(context: Context, option: number): number {
  try {
    return requireNative().ctxGetOpt(getNativeHandle(context), option | 0);
  } catch (error) {
    if (
      (option | 0) === ContextOption.THREAD_PRIORITY ||
      (option | 0) === ContextOption.THREAD_SCHED_POLICY
    ) {
      return -1;
    }
    throw createError('config', readErrno(), nativeErrorMessage(error, 'context option get failed'));
  }
}

function getContextOptionRawStrict(context: Context, option: number): number {
  try {
    return requireNative().ctxGetOpt(getNativeHandle(context), option | 0);
  } catch (error) {
    const message = error instanceof Error && error.message
      ? error.message
      : 'ctx_getopt failed';
    throw createError('config', readErrno(), message);
  }
}

export class Context extends NativeHandle {
  readonly options: ContextOptions;

  constructor() {
    super(requireNative().ctxNew());
    if (!this._native) throw lastError('config', 'context creation failed');
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

  close(): void {
    if (!this._native) return;
    closeCall('context close failed', () => {
      requireNative().ctxTerm(this._native);
    });
    this._native = null;
  }
}

export {
  Context as RuntimeContext,
  ContextOptions as RuntimeContextOptions,
};
