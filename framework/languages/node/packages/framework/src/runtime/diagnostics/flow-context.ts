import { AsyncLocalStorage } from 'node:async_hooks';
import { randomBytes } from 'node:crypto';
import type { ZLinkFlowOrigin } from '../../contracts';

export interface ZLinkFlowContextValue {
  readonly flowId: string;
  readonly flowOrigin: ZLinkFlowOrigin;
}

const flowStorage = new AsyncLocalStorage<ZLinkFlowContextValue | undefined>();

export function currentOrCreateFlow(origin?: ZLinkFlowOrigin, createIfAbsent?: true): ZLinkFlowContextValue;
export function currentOrCreateFlow(origin: ZLinkFlowOrigin, createIfAbsent: false): ZLinkFlowContextValue | undefined;
export function currentOrCreateFlow(origin: ZLinkFlowOrigin, createIfAbsent: boolean): ZLinkFlowContextValue | undefined;
export function currentOrCreateFlow(
  origin: ZLinkFlowOrigin = 'Application',
  createIfAbsent = true
): ZLinkFlowContextValue | undefined {
  const current = flowStorage.getStore();
  if (current !== undefined) {
    return current;
  }
  if (!createIfAbsent) {
    return undefined;
  }
  const created = { flowId: createFlowId(), flowOrigin: origin };
  flowStorage.enterWith(created);
  return created;
}

export function runWithFlow<T>(flow: ZLinkFlowContextValue | undefined, callback: () => T): T {
  return flowStorage.run(flow, callback);
}

export function createInboundFlow(
  flowId?: string,
  flowOrigin?: ZLinkFlowOrigin,
  createIfAbsent = true
): ZLinkFlowContextValue | undefined {
  if (flowId === undefined && !createIfAbsent) {
    return undefined;
  }
  return { flowId: flowId ?? createFlowId(), flowOrigin: flowOrigin ?? 'Inbound' };
}

function createFlowId(): string {
  const bytes = randomBytes(16);
  const timestamp = BigInt(Date.now());
  for (let index = 5; index >= 0; index -= 1) {
    bytes[index] = Number(timestamp >> BigInt((5 - index) * 8) & 0xffn);
  }
  bytes[6] = 0x70 | (bytes[6] & 0x0f);
  bytes[8] = 0x80 | (bytes[8] & 0x3f);
  const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}
