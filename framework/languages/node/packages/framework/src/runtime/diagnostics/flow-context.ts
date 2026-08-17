import { AsyncLocalStorage } from 'node:async_hooks';
import { randomBytes } from 'node:crypto';
import type { ZLinkFlowOrigin } from '../../contracts';

export interface ZLinkFlowContextValue {
  readonly flowId: string;
  readonly flowOrigin: ZLinkFlowOrigin;
}

const flowStorage = new AsyncLocalStorage<ZLinkFlowContextValue | undefined>();

export function currentFlowContext(): ZLinkFlowContextValue | undefined {
  return flowStorage.getStore();
}

export function currentOrCreateFlow(origin?: ZLinkFlowOrigin, createIfAbsent?: true): ZLinkFlowContextValue;
export function currentOrCreateFlow(origin: ZLinkFlowOrigin, createIfAbsent: false): ZLinkFlowContextValue | undefined;
export function currentOrCreateFlow(origin: ZLinkFlowOrigin, createIfAbsent: boolean): ZLinkFlowContextValue | undefined;
/**
 * Returns the ambient flow context or, when absent and `createIfAbsent`, a
 * fresh flow value. The returned value is NOT installed into the ambient
 * async context (spec 27 §4/§6: flow scopes are call-scoped; a top-level
 * outbound must not leave its flow behind in unrelated application work).
 * Outbound entry points that need an ambient scope wrap their operation in
 * {@link runWithOutboundFlow} instead.
 */
export function currentOrCreateFlow(
  origin: ZLinkFlowOrigin = 'Application',
  createIfAbsent = true
): ZLinkFlowContextValue | undefined {
  const current = currentFlowContext();
  if (current !== undefined) {
    return current;
  }
  if (!createIfAbsent) {
    return undefined;
  }
  return { flowId: createFlowId(), flowOrigin: origin };
}

export function runWithFlow<T>(flow: ZLinkFlowContextValue | undefined, callback: () => T): T {
  if (flow === undefined && currentFlowContext() === undefined) {
    return callback();
  }
  return flowStorage.run(flow, callback);
}

/**
 * Call-scoped outbound flow scope shared by every outbound entry point
 * (channel operations, mesh/route transports, spot address transport,
 * native-fallback bound session). When tracing is enabled and no ambient
 * flow exists, installs a fresh Application flow for the duration of
 * `callback` only — the caller's async context is untouched afterwards
 * (spec 27 §4). Envelope encoders and trace points inside the callback
 * observe the same ambient flow, so the source-side hop stays coherent.
 */
export function runWithOutboundFlow<T>(enabled: boolean, callback: () => T): T {
  if (!enabled || currentFlowContext() !== undefined) {
    return callback();
  }
  return flowStorage.run({ flowId: createFlowId(), flowOrigin: 'Application' }, callback);
}

export function createInboundFlow(
  flowId?: string,
  flowOrigin?: ZLinkFlowOrigin,
  createIfAbsent = true
): ZLinkFlowContextValue | undefined {
  if (!createIfAbsent) {
    return undefined;
  }
  return { flowId: flowId ?? createFlowId(), flowOrigin: flowOrigin ?? 'Inbound' };
}

function createFlowId(): string {
  const bytes = randomBytes(16);
  // Date.now() is always within 48 bits, which is exactly the UUIDv7
  // timestamp field.
  bytes.writeUIntBE(Date.now(), 0, 6);
  bytes[6] = 0x70 | (bytes[6] & 0x0f);
  bytes[8] = 0x80 | (bytes[8] & 0x3f);
  const hex = bytes.toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
