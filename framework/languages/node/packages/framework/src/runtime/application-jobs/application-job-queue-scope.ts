import { AsyncLocalStorage } from 'node:async_hooks';

import type { ApplicationJobPermitPort } from './contracts';

interface ApplicationJobPermitScope {
  readonly permit: ApplicationJobPermitPort;
  detached: boolean;
}

const applicationJobPermitScope = new AsyncLocalStorage<ApplicationJobPermitScope>();

/**
 * Carries the host-owned queue permit through internal async dispatch without
 * leaking capacity ownership into public handler contracts.
 */
export async function runWithApplicationJobPermit<TResult>(
  permit: ApplicationJobPermitPort,
  callback: () => Promise<TResult> | TResult
): Promise<TResult> {
  const scope: ApplicationJobPermitScope = { permit, detached: false };
  return applicationJobPermitScope.run(scope, async () => {
    try {
      return await callback();
    } finally {
      if (!scope.detached) permit.releaseAfterInternalProcessing();
    }
  });
}

/** Releases the current job permit immediately before the first user callback. */
export function releaseApplicationJobPermitBeforeHandler(): void {
  applicationJobPermitScope.getStore()?.permit.releaseBeforeHandler();
}

/** @internal Returns an ingress permit after a durable queue takes record ownership. */
export function releaseApplicationJobPermitForDurableHandoff(): void {
  applicationJobPermitScope.getStore()?.permit.releaseAfterInternalProcessing();
}

/** True while dispatch already owns the host-wide application job permit. */
export function hasApplicationJobPermit(): boolean {
  return applicationJobPermitScope.getStore() !== undefined;
}

/** Re-enters the captured permit scope when a serial executor runs later. */
export function bindApplicationJobPermit<TResult>(
  callback: () => Promise<TResult> | TResult
): () => Promise<TResult> | TResult {
  const scope = applicationJobPermitScope.getStore();
  if (scope === undefined) return callback;
  return () => applicationJobPermitScope.run(scope, callback);
}

/**
 * Transfers the current permit to a detached internal handler turn. The
 * detached owner must release it at handler entry or at a pre-start terminal.
 */
export function detachApplicationJobPermit(): ApplicationJobPermitPort | undefined {
  const scope = applicationJobPermitScope.getStore();
  if (scope === undefined) return undefined;
  scope.permit.detachForHandlerTurn?.();
  scope.detached = true;
  return scope.permit;
}
