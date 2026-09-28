import type { ActorRef, ZLinkActorJoinOperationId } from '../../contracts';

/**
 * The Accepted completion of a deferred cross-node Join. Spot Actor membership
 * sections 4-5: the public OperationId, the optional reply and the delivery
 * state live only while the current source and target processes run. The
 * completion is never written to a Store and never replayed after a restart.
 */
export interface ZLinkDeferredJoinCompletion {
  readonly operationId: ZLinkActorJoinOperationId;
  readonly actor: ActorRef;
  readonly rawReply: Buffer;
  readonly replyContentType?: string;
}

export function createDeferredJoinCompletion(
  actorId: string,
  operationId: ZLinkActorJoinOperationId,
  actor: ActorRef,
  rawReply: Uint8Array,
  replyContentType?: string
): ZLinkDeferredJoinCompletion {
  if (
    actorId.length === 0 ||
    actor.actorId !== actorId ||
    actor.objectGeneration <= 0n ||
    (operationId.high === 0n && operationId.low === 0n)
  ) {
    throw new Error('Deferred Join completion identity is invalid.');
  }
  if (replyContentType !== undefined && replyContentType.length === 0) {
    throw new TypeError('Deferred Join reply content type must be non-empty.');
  }
  return {
    operationId,
    actor,
    rawReply: Buffer.from(rawReply),
    ...(replyContentType === undefined ? {} : { replyContentType })
  };
}
