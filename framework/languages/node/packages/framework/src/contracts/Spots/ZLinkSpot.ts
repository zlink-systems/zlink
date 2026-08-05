import type { ZLinkActor } from '../Actors';
import type { ZLinkMessage } from '../Common';
import type {
  ZLinkEntrySpotContext,
  ZLinkInstanceSpotContext,
  ZLinkSpotContext
} from './Contracts';

export interface ZLinkSpotAcceptRejectResponse {
  readonly accepted: boolean;
  readonly reply?: unknown;
}

export interface ZLinkSpotActorJoinResult extends ZLinkSpotAcceptRejectResponse {}

export interface ZLinkSpotCreateResponse extends ZLinkSpotAcceptRejectResponse {}
export interface ZLinkActorCreateResponse extends ZLinkSpotAcceptRejectResponse {}

export enum ZLinkSpotCloseReason {
  ExplicitClose = 0,
  HostShutdown = 1,
  RelocationOut = 2,
  IdleEvicted = 3
}

export interface ZLinkSpotClosingContext {
  readonly reason: ZLinkSpotCloseReason;
  readonly deadline: Date;
}

export enum ZLinkSpotRelocationReadyOutcome {
  Continued = 0,
  Relocated = 1
}

export interface ZLinkSpotRelocationReadyCompletion {
  readonly outcome: ZLinkSpotRelocationReadyOutcome;
}

export interface ZLinkSpotRelocationReadyCall {
  defer(): void;
}

export interface ZLinkSpotActorMembershipLifecycle<
  TActor extends ZLinkActor = ZLinkActor
> {
  onJoinedActor(actor: TActor): Promise<void>;
  onLeaveActor(actor: TActor): Promise<void>;
  onDisconnectActor?(actor: TActor): Promise<void>;
}

export interface ZLinkUserSpotActorLifecycle<
  TActor extends ZLinkActor = ZLinkActor
> extends ZLinkSpotActorMembershipLifecycle<TActor> {
  // Admission observes the joining Actor by identity only. The framework keeps
  // fencing state such as the expected membership epoch inside the runtime so an
  // application callback never has to reason about it.
  onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
}

export interface ZLinkSpot<TActor extends ZLinkActor = ZLinkActor>
  extends ZLinkUserSpotActorLifecycle<TActor> {
  readonly context: ZLinkSpotContext<TActor>;
  configure?(): void;
  onCreate?(request: ZLinkMessage): Promise<ZLinkSpotCreateResponse>;
  onInitialize?(): Promise<void>;
  onClosing?(context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal): Promise<void>;
  onRelocationReadyCompleted?(completion: ZLinkSpotRelocationReadyCompletion): Promise<void>;
}

export interface ZLinkInstanceSpot {
  readonly context: ZLinkInstanceSpotContext;
  configure?(): void;
  onInitialize?(): Promise<void>;
  onClosing?(context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal): Promise<void>;
}

export interface ZLinkEntrySpot<TActor extends ZLinkActor = ZLinkActor>
  extends ZLinkSpotActorMembershipLifecycle<TActor> {
  readonly context: ZLinkEntrySpotContext<TActor>;
  configure?(): void;
  onInitialize?(): Promise<void>;
  onClosing?(context: ZLinkSpotClosingContext, cleanupSignal: AbortSignal): Promise<void>;
  onCreateActor?(
    actor: TActor,
    createRequest: ZLinkMessage
  ): Promise<ZLinkActorCreateResponse>;
}
