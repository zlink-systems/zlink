import type {
  ActorRef,
  ZLinkActor,
  ZLinkActorMembership
} from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';

export const ZLINK_ACTOR_LIFECYCLE_SNAPSHOT = Symbol('zlink.actor.lifecycle-snapshot');

export interface ZLinkActorLifecycleSnapshotSource {
  readonly actorRef: ActorRef;
  readonly actorType: string;
  readonly membershipEpoch: bigint;
}

interface ZLinkActorLifecycleSnapshotContext {
  readonly actorId?: string;
  readonly [ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]?: () => ZLinkActorLifecycleSnapshotSource;
}

interface ZLinkActorJoinRequest {
  readonly actor: ActorRef;
  readonly actorType: string;
  readonly expectedMembershipEpoch: bigint;
}

/**
 * Identity an admission callback observes. Join admission decides on the Actor's
 * identity alone, so the runtime resolves it from the same framework snapshot the
 * membership values use instead of handing the application the fencing state.
 */
export function actorJoinIdentity(actor: ZLinkActor): string {
  return lifecycleSource(actor).actorRef.actorId;
}

export function createActorJoinRequest(
  actor: ZLinkActor,
  actorRef?: ActorRef,
  expectedMembershipEpoch?: bigint
): ZLinkActorJoinRequest {
  const source = lifecycleSource(actor);
  return Object.freeze({
    actor: immutableActorRef(actorRef ?? source.actorRef),
    actorType: source.actorType,
    expectedMembershipEpoch: expectedMembershipEpoch ?? source.membershipEpoch
  });
}

export function createActorMembership(
  actor: ZLinkActor,
  actorRef?: ActorRef,
  membershipEpoch?: bigint
): ZLinkActorMembership {
  const source = lifecycleSource(actor);
  return Object.freeze({
    actor: immutableActorRef(actorRef ?? source.actorRef),
    actorType: source.actorType,
    membershipEpoch: membershipEpoch ?? source.membershipEpoch
  });
}

function lifecycleSource(actor: ZLinkActor): ZLinkActorLifecycleSnapshotSource {
  const rawContext: unknown = actor.context;
  const context = typeof rawContext === 'object' && rawContext !== null
    ? (rawContext as ZLinkActorLifecycleSnapshotContext)
    : undefined;
  // A caller can hand in a value that carries no Framework context at all, so the
  // failure path reports the configuration error instead of dereferencing it.
  const actorId = context?.actorId ?? '<unknown>';
  const snapshot = context?.[ZLINK_ACTOR_LIFECYCLE_SNAPSHOT];
  const source = snapshot === undefined ? undefined : snapshot.call(context);
  if (source === undefined) {
    throw new ZLinkConfigurationException(
      `Actor '${actorId}' does not expose a framework lifecycle identity snapshot.`
    );
  }
  if (source.actorRef.actorId !== actorId || source.actorType.length === 0) {
    throw new ZLinkConfigurationException(`Actor '${actorId}' lifecycle identity is invalid.`);
  }
  return source;
}

function immutableActorRef(actorRef: ActorRef): ActorRef {
  return Object.freeze({
    actorId: actorRef.actorId,
    objectGeneration: actorRef.objectGeneration,
    meshName: actorRef.meshName,
    nodeRid: actorRef.nodeRid
  });
}
