import { AsyncLocalStorage } from 'node:async_hooks';

import type { ApplicationJobOrigin, ApplicationJobPermitPort } from './contracts';

const applicationJobPermitScope = new AsyncLocalStorage<ApplicationJobPermitPort | undefined>();

function releasePermitTurn(): void {
  releaseApplicationJobPermitTurn(applicationJobPermitScope.getStore());
}

/** Ends the internal processing of one record turn unless its handler detached the permit. */
export function releaseApplicationJobPermitTurn(
  permit: ApplicationJobPermitPort | undefined
): void {
  if (permit !== undefined && !permit.detachedForHandlerTurn)
    permit.releaseAfterInternalProcessing();
}

/**
 * Makes `permit` the current record's permit for the synchronous part of a
 * dispatch. The caller restores the returned previous scope with
 * {@link leaveApplicationJobPermitScope} and releases the turn itself, so a
 * dispatcher that already continues on the handler result needs no second
 * continuation here.
 */
export function enterApplicationJobPermitScope(
  permit: ApplicationJobPermitPort
): ApplicationJobPermitPort | undefined {
  const previous = applicationJobPermitScope.getStore();
  applicationJobPermitScope.enterWith(permit);
  return previous;
}

export function leaveApplicationJobPermitScope(
  previous: ApplicationJobPermitPort | undefined
): void {
  applicationJobPermitScope.enterWith(previous);
}

function releasePermitSuccess<TResult>(value: TResult): TResult {
  releasePermitTurn();
  return value;
}

function releasePermitFailure(error: unknown): never {
  releasePermitTurn();
  throw error;
}

/**
 * Carries the host-owned queue permit through internal async dispatch without
 * leaking capacity ownership into public handler contracts.
 */
export function runWithApplicationJobPermit<TResult>(
  permit: ApplicationJobPermitPort,
  callback: () => Promise<TResult>
): Promise<TResult>;
export function runWithApplicationJobPermit<TResult, TFirst = undefined, TSecond = undefined>(
  permit: ApplicationJobPermitPort,
  callback: (first: TFirst, second: TSecond) => Promise<TResult> | TResult,
  first?: TFirst,
  second?: TSecond
): TResult | Promise<TResult>;
export function runWithApplicationJobPermit<TResult, TFirst, TSecond>(
  permit: ApplicationJobPermitPort,
  callback: (first: TFirst, second: TSecond) => Promise<TResult> | TResult,
  first?: TFirst,
  second?: TSecond
): TResult | Promise<TResult> {
  const previous = applicationJobPermitScope.getStore();
  applicationJobPermitScope.enterWith(permit);
  try {
    const result = callback(first as TFirst, second as TSecond);
    if (result !== null && (typeof result === 'object' || typeof result === 'function')) {
      return Promise.resolve(result).then(releasePermitSuccess, releasePermitFailure);
    }
    releasePermitTurn();
    return result;
  } catch (error) {
    releasePermitTurn();
    return Promise.reject(error);
  } finally {
    applicationJobPermitScope.enterWith(previous);
  }
}

/** Releases the current job permit immediately before the first user callback. */
export function releaseApplicationJobPermitBeforeHandler(): void {
  applicationJobPermitScope.getStore()?.releaseBeforeHandler();
}

/** @internal Returns an ingress permit after a durable queue takes record ownership. */
export function releaseApplicationJobPermitForDurableHandoff(): void {
  applicationJobPermitScope.getStore()?.releaseAfterInternalProcessing();
}

/** True while dispatch already owns the host-wide application job permit. */
export function hasApplicationJobPermit(): boolean {
  return applicationJobPermitScope.getStore() !== undefined;
}

/** Keeps the ingress origin when a durable record later reacquires its permit. */
export function currentApplicationJobOrigin(): ApplicationJobOrigin {
  return applicationJobPermitScope.getStore()!.origin;
}

/** Re-enters the captured permit scope when a serial executor runs later. */
export function bindApplicationJobPermit<TResult, TArgument>(
  callback: (argument?: TArgument) => Promise<TResult> | TResult
): (argument?: TArgument) => Promise<TResult> | TResult {
  const scope = applicationJobPermitScope.getStore();
  if (scope === undefined) return callback;
  return (argument?: TArgument) => applicationJobPermitScope.run(scope, callback, argument);
}

/**
 * Transfers the current permit to a detached internal handler turn. The
 * detached owner must release it at handler entry or at a pre-start terminal.
 */
export function detachApplicationJobPermit(): ApplicationJobPermitPort | undefined {
  const permit = applicationJobPermitScope.getStore();
  if (permit === undefined) return undefined;
  permit.detachForHandlerTurn();
  return permit;
}
