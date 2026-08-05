import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import {
  ZLinkFrameworkException,
  type ActorRef,
  type RoutingId
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
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

interface ZLinkSpotActorPacketRelayDispatchOptions {
  readonly actorPacketHandler?: (
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<unknown>;
  readonly actorPacketTargetProvider?: (actorId: string) => ZLinkRemoteActorPacketTarget | undefined;
  readonly bindRemoteSession?: (
    actor: ActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId,
    declaredTarget?: ZLinkRemoteBoundSessionTarget
  ) => void;
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
      const frameHeader = decodeStreamHeader(messageToBytes(header));
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
      const response = await this.options.actorPacketHandler(
        actorPacketRelay.actorId,
        [header, payload],
        true,
        remoteBoundSessionTarget,
        actorPacketRelay.actorRef
      );
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
