import type {
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkSpotActorJoinResult
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ZLinkBackendReceived as BackendReceived } from '../backend/runtime-values';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  type ZLinkChannelEnvelopeCodecRegistry
} from '../channels/channel-envelope';
import {
  encodeFrameworkPayloadMessage,
  wrapFrameworkPayloadMessage
} from '../messaging/payload-codec';
import { routingIdWireHex as encodeRoutingIdHex } from '../routing-id';
import {
  REMOTE_ACTOR_JOIN_PACKET,
  decodeRemoteActorJoinPayload,
  hasRemoteActorJoinIdentity,
  isRemoteActorJoinPayload,
  type ZLinkDecodedRemoteActorJoinRequest,
  type ZLinkRemoteActorJoinWirePayload
} from './spot-remote-codec';
import { submitRoutedActorJoinError, submitRoutedActorJoinReply } from './spot-route-replies';
import type { ZLinkSpotSerialTurnExecutor } from './spot-serial-turn-executor';
const DEFAULT_PENDING_ADMISSION_TIMEOUT_MS = 30_000;

interface ZLinkRoutedActorAdmissionTarget {
  onActorJoin?(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
}

interface ZLinkSpotRoutedActorAdmissionOptions {
  readonly serial: ZLinkSpotSerialTurnExecutor;
  readonly getTarget: () => ZLinkRoutedActorAdmissionTarget;
  readonly defaultAccept: boolean;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly pendingAdmissionTimeoutMs?: number;
  /** Spec 27 §4: with tracing Off, inbound flow fields are neither validated nor carried forward. */
  readonly flowEnabled?: () => boolean;
}

interface ZLinkPendingRoutedActorTransfer {
  readonly actorId: string;
  readonly actorType: string;
  admissionTask?: Promise<Record<string, unknown>>;
  admissionReply?: Record<string, unknown>;
  deadline?: ReturnType<typeof setTimeout>;
}

export class ZLinkSpotRoutedActorAdmission {
  private readonly pending = new Map<string, ZLinkPendingRoutedActorTransfer>();

  constructor(private readonly options: ZLinkSpotRoutedActorAdmissionOptions) {}

  async admit(received: BackendReceived): Promise<boolean> {
    if (received.replyToken === null) {
      return false;
    }
    const decoded = this.decodeRemoteActorJoinRequest(received.parts);
    if (decoded === undefined) {
      return false;
    }
    if (decoded.phase === 'admission') {
      await this.admitTransfer(decoded, received);
      return true;
    }
    try {
      submitRoutedActorJoinError(
        received,
        decoded,
        new Error('Remote actor join requires the admission phase.')
      );
    } finally {
      this.closeDecoded(decoded);
    }
    return true;
  }

  private async admitTransfer(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    received: BackendReceived
  ): Promise<void> {
    try {
      if (decoded.transferId === undefined) {
        throw new Error('Remote actor transfer admission requires a transfer id.');
      }
      const existing = this.pending.get(decoded.transferId);
      if (existing !== undefined) {
        this.requireMatchingTransfer(existing, decoded);
        const reply = existing.admissionReply ?? (await existing.admissionTask);
        if (reply === undefined)
          throw new Error(`Remote actor transfer '${decoded.transferId}' has no admission result.`);
        submitRoutedActorJoinReply(received, decoded, reply);
        return;
      }
      const pending: ZLinkPendingRoutedActorTransfer = {
        actorId: decoded.actorId,
        actorType: decoded.actorType
      };
      this.pending.set(decoded.transferId, pending);
      pending.admissionTask = this.evaluateAdmission(decoded, pending);
      const admissionReply = await pending.admissionTask;
      submitRoutedActorJoinReply(received, decoded, admissionReply);
    } catch (error) {
      submitRoutedActorJoinError(received, decoded, error);
    } finally {
      this.closeDecoded(decoded);
    }
  }

  private async evaluateAdmission(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    pending: ZLinkPendingRoutedActorTransfer
  ): Promise<Record<string, unknown>> {
    try {
      const response = await this.runJoinCallback(decoded, decoded.request);
      const reply =
        response.reply === undefined
          ? undefined
          : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers);
      try {
        const actorRef = decoded.actorRef;
        const admissionReply = {
          accepted: response.accepted,
          actorNodeRid: String(actorRef?.nodeRid ?? ''),
          actorNodeRidHex:
            actorRef?.nodeRid === undefined ? undefined : encodeRoutingIdHex(actorRef.nodeRid),
          actorId: decoded.actorId,
          actorGeneration: (actorRef?.generation ?? 0n).toString(),
          reply: reply?.data().toString('base64')
        };
        pending.admissionReply = admissionReply;
        this.scheduleExpiration(decoded.transferId!, pending);
        return admissionReply;
      } finally {
        reply?.close();
      }
    } catch (error) {
      this.scheduleExpiration(decoded.transferId!, pending);
      throw error;
    }
  }

  private runJoinCallback(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    request: Message
  ): Promise<ZLinkSpotActorJoinResult> {
    const target = this.options.getTarget();
    const joinPayload = wrapFrameworkPayloadMessage(
      request,
      this.options.messageSerializers,
      decoded.requestContentType
    );
    const actorRef = decoded.actorRef;
    if (actorRef === undefined) {
      throw new Error(`Remote actor '${decoded.actorId}' join identity is missing its ActorRef.`);
    }
    return this.options.serial.execute(async () =>
      this.options.defaultAccept || target.onActorJoin === undefined
        ? { accepted: this.options.defaultAccept }
        : target.onActorJoin(actorRef.actorId, joinPayload)
    );
  }

  private requireMatchingTransfer(
    pending: ZLinkPendingRoutedActorTransfer,
    decoded: ZLinkDecodedRemoteActorJoinRequest
  ): void {
    if (pending.actorId !== decoded.actorId || pending.actorType !== decoded.actorType) {
      throw new Error(
        `Remote actor transfer '${decoded.transferId}' identity does not match its first request.`
      );
    }
  }

  private scheduleExpiration(transferId: string, pending: ZLinkPendingRoutedActorTransfer): void {
    if (pending.deadline !== undefined) clearTimeout(pending.deadline);
    pending.deadline = setTimeout(() => {
      if (this.pending.get(transferId) === pending) this.pending.delete(transferId);
    }, this.options.pendingAdmissionTimeoutMs ?? DEFAULT_PENDING_ADMISSION_TIMEOUT_MS);
    pending.deadline.unref();
  }

  private closeDecoded(decoded: ZLinkDecodedRemoteActorJoinRequest): void {
    decoded.request.close();
    decoded.actorCreateRequest?.close();
  }

  private decodeRemoteActorJoinRequest(
    parts: readonly Message[]
  ): ZLinkDecodedRemoteActorJoinRequest | undefined {
    if (parts.length < 2 || parts[0].data().length === 0) {
      if (parts.length !== 1 || parts[0].data().length === 0) {
        return undefined;
      }
      try {
        const payload = JSON.parse(parts[0].data().toString()) as ZLinkRemoteActorJoinWirePayload;
        if (payload.packetName !== REMOTE_ACTOR_JOIN_PACKET || !isRemoteActorJoinPayload(payload)) {
          return undefined;
        }
        return decodeRemoteActorJoinPayload(
          payload,
          RuntimeMessage.from(Buffer.from(payload.request, 'base64')),
          true
        );
      } catch {
        return undefined;
      }
    }
    try {
      const envelope = decodeChannelEnvelope(
        parts,
        undefined,
        this.options.flowEnabled?.() ?? true
      );
      if (envelope.packetName !== REMOTE_ACTOR_JOIN_PACKET) {
        return undefined;
      }
      const payload = decodeChannelPayload(envelope, this.channelCodecs());
      if (!isRemoteActorJoinPayload(payload)) {
        return undefined;
      }
      return decodeRemoteActorJoinPayload(
        payload,
        RuntimeMessage.from(Buffer.from(payload.request, 'base64')),
        false,
        envelope
      );
    } catch {
      try {
        const header = JSON.parse(parts[0].data().toString()) as ZLinkRemoteActorJoinWirePayload;
        if (
          header.packetName !== REMOTE_ACTOR_JOIN_PACKET ||
          !hasRemoteActorJoinIdentity(header) ||
          parts.length < 2
        ) {
          return undefined;
        }
        return decodeRemoteActorJoinPayload(
          header,
          RuntimeMessage.fromOwned(Buffer.from(parts[1].data())),
          true
        );
      } catch {
        return undefined;
      }
    }
  }

  private channelCodecs(): ZLinkChannelEnvelopeCodecRegistry | undefined {
    return this.options.messageSerializers === undefined
      ? undefined
      : { serializers: this.options.messageSerializers };
  }
}
