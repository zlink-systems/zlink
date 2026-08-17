import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import {
  ZLinkFrameworkException,
  type ActorRef,
  type RoutingId
} from '../../contracts';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ZLinkBackendReceived as BackendReceived } from '../backend/runtime-values';
import type {
  ZLinkRemoteActorPacketTarget,
  ZLinkRemoteBoundSessionTarget
} from '../actors';
import {
  decodeStreamHeader,
  messageToBytes,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import {
  decodeRemoteActorSessionBinding,
  encodeRemoteActorPacketTarget,
  ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET
} from '../actors/actor-packet-relay-wire';
import { decodeRoutingId as decodeWireRoutingId, routingIdsEqual } from '../routing-id';
import type { ZLinkRemoteActorPacketRelay } from './spot-remote-route-codec';
import {
  isReplyableRequestSeq,
  submitSpotRouteBridgeReply
} from './spot-route-replies';
import type { ZLinkActorPacketDelivery } from './spot-actor-packet-dispatch';

interface ZLinkSpotActorPacketRelayDispatchOptions {
  readonly actorPacketHandler?: (delivery: ZLinkActorPacketDelivery) => Promise<unknown>;
  readonly actorPacketTargetProvider?: (actorId: string) => ZLinkRemoteActorPacketTarget | undefined;
  readonly bindRemoteSession?: (
    actor: ActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId,
    declaredTarget?: ZLinkRemoteBoundSessionTarget
  ) => void;
  readonly flowEnabled?: () => boolean;
}

export class ZLinkSpotActorPacketRelayDispatch {
  constructor(private readonly options: ZLinkSpotActorPacketRelayDispatchOptions) {}

  async dispatch(
    received: BackendReceived,
    actorPacketRelay: ZLinkRemoteActorPacketRelay
  ): Promise<boolean> {
    if (this.options.actorPacketHandler === undefined) {
      return false;
    }
    const remoteBoundSessionTarget =
      actorPacketRelay.routerChannelId === undefined ||
      actorPacketRelay.boundSessionTargetNodeRid === undefined ||
      actorPacketRelay.boundSessionSpotId === undefined
      ? undefined
      : {
          routerChannelId: actorPacketRelay.routerChannelId,
          targetNodeRid: decodeWireRoutingId(
            actorPacketRelay.boundSessionTargetNodeRid,
            undefined
          ),
          spotId: decodeWireRoutingId(
            actorPacketRelay.boundSessionSpotId,
            undefined
          )
        };
    const header = RuntimeMessage.from(Buffer.from(actorPacketRelay.header, 'base64'));
    const payload = RuntimeMessage.from(Buffer.from(actorPacketRelay.payload, 'base64'));
    try {
      const frameHeader = decodeStreamHeader(
        messageToBytes(header),
        this.options.flowEnabled?.() ?? true
      );
      if (frameHeader.name === ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET) {
        const actorRef = actorPacketRelay.bindingActorRef;
        const sourceNodeRid = received.routingId as RoutingId | null;
        if (
          frameHeader.kind !== ZLinkStreamMessageKind.Send ||
          actorRef === undefined ||
          sourceNodeRid === null ||
          this.options.bindRemoteSession === undefined
        ) {
          throw new Error('Remote actor session binding requires a send, source route, and concrete actor ref.');
        }
        const binding = decodeRemoteActorSessionBinding(messageToBytes(payload));
        const declaredSourceNodeRid = binding.sessionNodeRid;
        if (!routingIdsEqual(sourceNodeRid, declaredSourceNodeRid)) {
          throw new Error('Remote actor session binding source did not match the declared session node.');
        }
        this.options.bindRemoteSession(
          actorRef,
          declaredSourceNodeRid,
          binding.sessionRid,
          remoteBoundSessionTarget === undefined
            ? undefined
            : {
                ...remoteBoundSessionTarget,
                sessionNodeRid: declaredSourceNodeRid,
                sessionRid: binding.sessionRid
              }
        );
        if (isReplyableRequestSeq(received.requestSeq)) {
          submitSpotRouteBridgeReply(received, actorPacketRelay.envelope, {
            ok: true,
            response: { acknowledged: true }
          });
        }
        return true;
      }
      if (actorPacketRelay.messageFollowContext?.deadlineUnixMs !== undefined
        && Date.now() >= actorPacketRelay.messageFollowContext.deadlineUnixMs) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
          `Actor request '${actorPacketRelay.actorId}' expired before target handler admission.`
        );
      }
      //  Spec 28 §5.2/§10 — a Message Follow relay is a one-way send that
      //  preserves the ORIGINAL bound-session reply route. When the outer
      //  transport cannot carry a bridge reply for such a followed request,
      //  the inner dispatch must deliver its terminal completion (response
      //  or error) through that original route via actorResponseSender/
      //  actorErrorSender instead of returning it locally, where it would
      //  be discarded. Non-follow relays keep the synchronous-return path.
      const outerReplyable = isReplyableRequestSeq(received.requestSeq);
      const followedBoundSessionReply =
        !outerReplyable
        && actorPacketRelay.messageFollowContext !== undefined
        && remoteBoundSessionTarget !== undefined;
      const response = await this.options.actorPacketHandler({
        actorId: actorPacketRelay.actorId,
        parts: [header, payload],
        returnResponse: !followedBoundSessionReply,
        remoteBoundSessionTarget,
        fallbackActorRef: actorPacketRelay.actorRef
      });
      const actorPacketTarget = this.actorPacketTarget(actorPacketRelay.actorId);
      if (isReplyableRequestSeq(received.requestSeq)) {
        submitSpotRouteBridgeReply(received, actorPacketRelay.envelope, { ok: true, response, actorPacketTarget });
      }
      return true;
    } catch (error) {
      if (isReplyableRequestSeq(received.requestSeq)) {
        submitSpotRouteBridgeReply(received, actorPacketRelay.envelope, {
          ok: false,
          error: error instanceof Error ? error.message : String(error),
          errorKind: error instanceof ZLinkFrameworkException ? error.kind : undefined
        });
        return true;
      }
      throw error;
    } finally {
      header.close();
      payload.close();
    }
  }

  private actorPacketTarget(actorId: string): ReturnType<typeof encodeRemoteActorPacketTarget> {
    return encodeRemoteActorPacketTarget(this.options.actorPacketTargetProvider?.(actorId));
  }
}
