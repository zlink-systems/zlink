import { AsyncLocalStorage } from 'node:async_hooks';
import { randomBytes } from 'node:crypto';
import type { ZLinkFlowOrigin } from '../../contracts';
const FLOW_UUID_BYTES = 16;

/**
 * Spec 27 §3 defines the flow_origin value set as lowercase
 * (`inbound|timer|application|lifecycle`); every language emits these exact
 * strings so records remain comparable across runtimes (spec 26 §3.1).
 */
export const FLOW_ORIGIN_VALUES: Readonly<Record<ZLinkFlowOrigin, string>> = Object.freeze({
  Inbound: 'inbound',
  Timer: 'timer',
  Application: 'application',
  Lifecycle: 'lifecycle'
});

export interface ZLinkFlowContextValue {
  readonly flowId: string;
  readonly flowOrigin: ZLinkFlowOrigin;
  readonly streamSessionId?: string;
}

const flowStorage = new AsyncLocalStorage<ZLinkFlowContextValue | undefined>();

export function currentFlowContext(): ZLinkFlowContextValue | undefined {
  return flowStorage.getStore();
}

export function currentOrCreateFlow(
  origin?: ZLinkFlowOrigin,
  createIfAbsent?: true
): ZLinkFlowContextValue;
export function currentOrCreateFlow(
  origin: ZLinkFlowOrigin,
  createIfAbsent: false
): ZLinkFlowContextValue | undefined;
export function currentOrCreateFlow(
  origin: ZLinkFlowOrigin,
  createIfAbsent: boolean
): ZLinkFlowContextValue | undefined;
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
  flowId: string | undefined,
  flowOrigin: ZLinkFlowOrigin | undefined,
  createIfAbsent: boolean,
  streamSessionId: string | undefined
): ZLinkFlowContextValue | undefined {
  if (!createIfAbsent) {
    return undefined;
  }
  return { flowId: flowId ?? createFlowId(), flowOrigin: flowOrigin ?? 'Inbound', streamSessionId };
}

/**
 * UUIDv7 with a dedicated counter (RFC 9562 §6.2 method 1): the 48-bit
 * millisecond timestamp, then a 42-bit counter that starts at a random value
 * and spans `rand_a` and the top of `rand_b`, then 32 random bits drawn once
 * per process. Flow ids stay unique within the process without drawing
 * random bytes for every message.
 */
const flowRandom = randomBytes(FLOW_UUID_BYTES);
const FLOW_COUNTER_SPAN = 2 ** 42;
let flowCounter = flowRandom.readUIntBE(0, 6) % FLOW_COUNTER_SPAN;
const FLOW_ID_TAIL = flowRandom.toString('hex', FLOW_UUID_BYTES - 4);

function createFlowId(): string {
  const counter = flowCounter;
  flowCounter = (flowCounter + 1) % FLOW_COUNTER_SPAN;
  // Date.now() is always within 48 bits, which is exactly the UUIDv7
  // timestamp field.
  const timestamp = Date.now().toString(16).padStart(12, '0');
  const randA = Math.floor(counter / 2 ** 30); // 12 bits
  const randB = counter % 2 ** 30; // 30 bits after the 2 variant bits
  const variantHigh = (0x8000 | Math.floor(randB / 2 ** 16)).toString(16);
  const randBLow = (randB % 2 ** 16).toString(16).padStart(4, '0');
  return `${timestamp.slice(0, 8)}-${timestamp.slice(8)}-7${randA.toString(16).padStart(3, '0')}-${variantHigh}-${randBLow}${FLOW_ID_TAIL}`;
}
