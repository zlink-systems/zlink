import type { Type } from '../Common';
import type { ZLinkActor } from '../Actors';
import type { ZLinkInstanceSpot, ZLinkSpot } from '../Spots';

export enum ZLinkUserSpotExecutionMode {
  SpotWide = 'spot_wide',
  PerActor = 'per_actor'
}

export enum ZLinkSpotRelocationReadinessMode {
  AnyTurnBoundary = 'any_turn_boundary',
  ApplicationSignaled = 'application_signaled'
}

export interface ZLinkActorRelocationAdapter<TActor extends ZLinkActor> {
  capture(actor: TActor, signal: AbortSignal): Promise<Uint8Array>;
  restore(actor: TActor, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  /**
   * Optional base/delta capture capability (spec 15 §5). Either all four of
   * captureBase, captureDelta, restoreBase, and applyDelta are implemented or
   * none; a partial implementation is a configuration error before bind.
   */
  captureBase?(actor: TActor, signal: AbortSignal): Promise<Uint8Array>;
  captureDelta?(actor: TActor, signal: AbortSignal): Promise<Uint8Array>;
  restoreBase?(actor: TActor, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  applyDelta?(actor: TActor, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

export interface ZLinkSpotRelocationAdapter<
  TSpot extends ZLinkSpot | ZLinkInstanceSpot
> {
  capture(spot: TSpot, signal: AbortSignal): Promise<Uint8Array>;
  restore(spot: TSpot, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  /** See ZLinkActorRelocationAdapter — all four members or none. */
  captureBase?(spot: TSpot, signal: AbortSignal): Promise<Uint8Array>;
  captureDelta?(spot: TSpot, signal: AbortSignal): Promise<Uint8Array>;
  restoreBase?(spot: TSpot, payload: Uint8Array, signal: AbortSignal): Promise<void>;
  applyDelta?(spot: TSpot, payload: Uint8Array, signal: AbortSignal): Promise<void>;
}

/** The optional base/delta members — implemented all together or not at all. */
export const ZLINK_RELOCATION_BASE_DELTA_MEMBERS = [
  'captureBase',
  'captureDelta',
  'restoreBase',
  'applyDelta'
] as const;

/** True when the adapter instance registers the complete base/delta capability. */
export function zlinkRelocationAdapterHasBaseDelta(value: object): boolean {
  return countBaseDeltaMembers(value) === ZLINK_RELOCATION_BASE_DELTA_MEMBERS.length;
}

/**
 * Enforces the all-or-none base/delta rule on an adapter type before the
 * host binds a socket. A partial implementation is a configuration error.
 */
export function zlinkValidateRelocationAdapterBaseDelta(
  adapterType: { readonly prototype?: object; readonly name?: string },
  label: string
): void {
  const prototype = adapterType.prototype;
  if (prototype === undefined) return;
  const count = countBaseDeltaMembers(prototype);
  if (count !== 0 && count !== ZLINK_RELOCATION_BASE_DELTA_MEMBERS.length) {
    throw new Error(
      `${label} relocation adapter '${adapterType.name ?? 'anonymous'}' implements `
      + `${count} of the four base/delta members `
      + '(captureBase, captureDelta, restoreBase, applyDelta); '
      + 'implement all four or none.'
    );
  }
}

function countBaseDeltaMembers(value: object): number {
  return ZLINK_RELOCATION_BASE_DELTA_MEMBERS.filter(member =>
    typeof (value as Record<string, unknown>)[member] === 'function').length;
}

export interface ZLinkActorFactoryBuilder<TActor extends ZLinkActor> {
  disableRelocation(): void;
  recreateOnRelocation(): void;
  preserveStateWith(adapterType: Type<ZLinkActorRelocationAdapter<TActor>>): void;
}

export interface ZLinkUserSpotFactoryBuilder<TSpot extends ZLinkSpot> {
  stableTypeLimit(limit: number): this;
  executionMode(mode: ZLinkUserSpotExecutionMode): this;
  relocationReadiness(mode: ZLinkSpotRelocationReadinessMode): this;
  disableRelocation(): void;
  recreateOnRelocation(): void;
  preserveStateWith(adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>): void;
}

export interface ZLinkInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot> {
  stableTypeLimit(limit: number): this;
  disableRelocation(): void;
  recreateOnRelocation(): void;
  preserveStateWith(adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>): void;
}
