import type { ActorRef } from '../Common';
import type { ZLinkFrameworkErrorKind } from '../Errors';
import type { ZLinkMessage } from '../Common';
import type { ZLinkActor } from './ZLinkActor';
import type { ZLinkActorContext } from './ZLinkActorContext';

export interface ZLinkActorJoinOperationId {
  readonly high: bigint;
  readonly low: bigint;
}

export type ZLinkActorJoinCompletion =
  | {
      readonly status: 'accepted';
      readonly operationId: ZLinkActorJoinOperationId;
      readonly actor: ActorRef;
      readonly reply?: ZLinkMessage;
    }
  | {
      readonly status: 'rejected';
      readonly operationId: ZLinkActorJoinOperationId;
      readonly reply?: ZLinkMessage;
    }
  | {
      readonly status: 'failed';
      readonly operationId: ZLinkActorJoinOperationId;
      readonly kind: ZLinkFrameworkErrorKind;
    };

export interface ZLinkActorJoinCall<TSelf> {
  timeout(timeoutMs: number): TSelf;
  defer(): void;
}

export interface ZLinkActorJoinSpotCall extends ZLinkActorJoinCall<ZLinkActorJoinSpotCall> {}

export interface ZLinkActorJoinEntrySpotCall extends ZLinkActorJoinCall<ZLinkActorJoinEntrySpotCall> {}

export interface ZLinkActorFactory<TActor extends ZLinkActor = ZLinkActor> {
  create(context: ZLinkActorContext, signal?: AbortSignal): Promise<TActor>;
}
