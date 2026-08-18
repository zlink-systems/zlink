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
}

export interface ZLinkSpotRelocationAdapter<
  TSpot extends ZLinkSpot | ZLinkInstanceSpot
> {
  capture(spot: TSpot, signal: AbortSignal): Promise<Uint8Array>;
  restore(spot: TSpot, payload: Uint8Array, signal: AbortSignal): Promise<void>;
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
