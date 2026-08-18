export interface ZLinkRelocationStateAdapterLike<TInstance> {
  capture(instance: TInstance, signal: AbortSignal): Promise<Uint8Array>;
  restore(instance: TInstance, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

/** Captures the application state through the adapter. */
export async function captureRelocationAdapterState<TInstance>(
  adapter: ZLinkRelocationStateAdapterLike<TInstance>,
  instance: TInstance,
  signal: AbortSignal
): Promise<Buffer> {
  return Buffer.from(await adapter.capture(instance, signal));
}

/** Restores the application state through the adapter. */
export async function restoreRelocationAdapterState<TInstance>(
  adapter: ZLinkRelocationStateAdapterLike<TInstance>,
  instance: TInstance,
  payload: Uint8Array,
  signal: AbortSignal
): Promise<TInstance> {
  await adapter.restore(instance, payload, signal);
  return instance;
}
