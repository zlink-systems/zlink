import type {
  ZLinkActor,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type {
  ZLinkBackendActorJoinRequest,
  ZLinkBackendSpot
} from '../backend/contracts';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  encodeFrameworkPayloadMessage,
  wrapFrameworkPayloadMessage
} from '../messaging/payload-codec';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import { REMOTE_ACTOR_JOIN_PACKET } from './spot-remote-codec';

interface ZLinkNativeActorJoinAdmissionTarget {
  onActorJoin?(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
  onJoinedActor?(actor: ZLinkActor): Promise<void>;
}

interface ZLinkSpotNativeActorJoinAdmissionOptions {
  readonly nativeSpot: ZLinkBackendSpot;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly resolveActor: (actorId: string) => ZLinkActor | undefined;
  readonly getTarget: () => ZLinkNativeActorJoinAdmissionTarget;
  readonly defaultAccept: boolean;
  readonly commitAcceptedActor?: (actor: ZLinkActor) => Promise<void>;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
}

export class ZLinkSpotNativeActorJoinAdmission {
  constructor(private readonly options: ZLinkSpotNativeActorJoinAdmissionOptions) {}

  async admit(request: ZLinkBackendActorJoinRequest): Promise<void> {
    const actorId = request.info.targetActor.actorId;
    let accepted = false;
    let acceptedActor: ZLinkActor | undefined;
    let reply: Message | undefined;
    const decoded = this.decodeNativeActorJoinRequest(request.message);
    try {
      if (decoded !== undefined) {
        throw new Error('Remote actor join requires the two-phase routed transfer protocol.');
      }
      const actor = this.options.resolveActor(actorId);
      if (actor !== undefined) {
        const target = this.options.getTarget();
        const joinRequest = request.message;
        const joinPayload = wrapFrameworkPayloadMessage(joinRequest, this.options.messageSerializers);
        const response: ZLinkSpotActorJoinResult = await this.options.serial.execute(async () =>
          this.options.defaultAccept || target.onActorJoin === undefined
            ? { accepted: this.options.defaultAccept }
            : target.onActorJoin(actorId, joinPayload)
        );
        accepted = response.accepted;
        reply = response.reply === undefined
          ? undefined
          : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers);
        if (accepted) {
          acceptedActor = actor;
        }
      }
    } catch (error) {
      this.reportHandlerException(actorId, error);
      accepted = false;
      reply?.close();
      reply = undefined;
    }
    const operation = this.options.nativeSpot.replyActorJoin(request, accepted ? 0 : 1);
    let submitted = false;
    try {
      if (reply !== undefined) {
        operation.message(reply);
      }
      operation.submit();
      submitted = true;
    } finally {
      if (!submitted) {
        reply?.close();
      }
      decoded?.request.close();
      decoded?.actorCreateRequest?.close();
    }
    if (acceptedActor !== undefined) {
      try {
        // The native reply commits the actor's return to the Entry Spot. Publish
        // the immutable membership callback only after that commit so observers
        // never see an accepted membership before Core does.
        await this.options.commitAcceptedActor?.(acceptedActor);
      } catch (error) {
        this.reportHandlerException(actorId, error);
      }
    }
  }

  private decodeNativeActorJoinRequest(message: Message): {
    readonly actorType: string;
    readonly actorCreateRequest?: Message;
    readonly request: Message;
  } | undefined {
    try {
      const payload = JSON.parse(message.data().toString()) as {
        readonly packetName?: unknown;
        readonly actorType?: unknown;
        readonly actorNodeRid?: unknown;
        readonly actorNodeRidHex?: unknown;
        readonly actorGeneration?: unknown;
        readonly actorCreateRequest?: unknown;
        readonly request?: unknown;
      };
      if (
        payload.packetName !== REMOTE_ACTOR_JOIN_PACKET ||
        typeof payload.actorType !== 'string' ||
        typeof payload.request !== 'string'
      ) {
        return undefined;
      }
      return {
        actorType: payload.actorType,
        actorCreateRequest: typeof payload.actorCreateRequest === 'string'
          ? RuntimeMessage.from(Buffer.from(payload.actorCreateRequest, 'base64'))
          : undefined,
        request: RuntimeMessage.from(Buffer.from(payload.request, 'base64'))
      };
    } catch {
      return undefined;
    }
  }

  private reportHandlerException(actorId: string, error: unknown): void {
    this.options.dispatchErrors?.report({
      surface: ZLinkDispatchErrorSurface.SpotActor,
      messageKind: ZLinkDispatchMessageKind.ActorSend,
      reason: ZLinkDispatchErrorReason.HandlerException,
      action: ZLinkDispatchErrorAction.Drop,
      actorId,
      error
    });
  }
}
