import type {
  ZLinkActor,
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
  type ZLinkRoutedActorTransferProvider,
  type ZLinkRemoteActorJoinWirePayload
} from './spot-remote-codec';
import {
  submitRoutedActorJoinError,
  submitRoutedActorJoinReply
} from './spot-route-replies';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import type { ZLinkActorHandoffPacket, ZLinkActorHandoffResult } from '../actors/actor-handoff';

interface ZLinkRoutedActorAdmissionTarget {
  onActorJoin?(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult>;
}

interface ZLinkSpotRoutedActorAdmissionOptions {
  readonly serial: ZLinkSpotSerialExecutor;
  readonly getTarget: () => ZLinkRoutedActorAdmissionTarget;
  readonly defaultAccept: boolean;
  readonly routedActorTransferProvider?: ZLinkRoutedActorTransferProvider;
  readonly commitTransferredActor?: (
    actor: ZLinkActor,
    backlog: readonly ZLinkActorHandoffPacket[]
  ) => Promise<readonly ZLinkActorHandoffResult[]>;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly pendingAdmissionTimeoutMs?: number;
}

interface ZLinkPendingRoutedActorTransfer {
  readonly actorId: string;
  readonly actorType: string;
  phase: 'admitting' | 'admitted' | 'rejected' | 'committing' | 'committed';
  admissionTask?: Promise<Record<string, unknown>>;
  commitTask?: Promise<Record<string, unknown>>;
  admissionReply?: Record<string, unknown>;
  commitReply?: Record<string, unknown>;
  deadline?: ReturnType<typeof setTimeout>;
}

export class ZLinkSpotRoutedActorAdmission {
  private readonly pending = new Map<string, ZLinkPendingRoutedActorTransfer>();

  constructor(private readonly options: ZLinkSpotRoutedActorAdmissionOptions) {}

  async admit(received: BackendReceived): Promise<boolean> {
    if (received.requestSeq === null) {
      return false;
    }
    const decoded = this.decodeRemoteActorJoinRequest(received.parts, received);
    if (decoded === undefined) {
      return false;
    }
    if (decoded.phase === 'admission') {
      await this.admitTransfer(decoded, received);
      return true;
    }
    if (decoded.phase === 'commit') {
      await this.commitTransfer(decoded, received);
      return true;
    }
    try {
      submitRoutedActorJoinError(
        received,
        decoded,
        new Error('Remote actor join requires admission and commit phases.')
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
        const reply = existing.admissionReply ?? await existing.admissionTask;
        if (reply === undefined) throw new Error(`Remote actor transfer '${decoded.transferId}' has no admission result.`);
        submitRoutedActorJoinReply(received, decoded, reply);
        return;
      }
      const pending: ZLinkPendingRoutedActorTransfer = {
        actorId: decoded.actorId,
        actorType: decoded.actorType,
        phase: 'admitting'
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

  private async commitTransfer(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    received: BackendReceived
  ): Promise<void> {
    try {
      const transferId = decoded.transferId;
      if (transferId === undefined) {
        throw new Error('Remote actor transfer commit requires a transfer id.');
      }
      const pending = this.pending.get(transferId);
      if (pending === undefined || pending.actorId !== decoded.actorId || pending.actorType !== decoded.actorType) {
        throw new Error(`Remote actor transfer '${transferId}' does not have a matching admission.`);
      }
      if (pending.phase === 'rejected') {
        throw new Error(`Remote actor transfer '${transferId}' admission was rejected.`);
      }
      if (pending.commitTask === undefined) {
        if (pending.phase !== 'admitted') {
          throw new Error(`Remote actor transfer '${transferId}' is not ready to commit.`);
        }
        pending.phase = 'committing';
        if (pending.deadline !== undefined) clearTimeout(pending.deadline);
        pending.commitTask = this.evaluateCommit(decoded, transferId, pending);
      }
      const commitReply = pending.commitReply ?? await pending.commitTask;
      submitRoutedActorJoinReply(received, decoded, commitReply);
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
      const reply = response.reply === undefined
        ? undefined
        : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers);
      try {
        const actorRef = decoded.actorRef;
        const admissionReply = {
          accepted: response.accepted,
          actorNodeRid: String(actorRef?.nodeRid ?? ''),
          actorNodeRidHex: actorRef?.nodeRid === undefined ? undefined : encodeRoutingIdHex(actorRef.nodeRid),
          actorId: decoded.actorId,
          actorGeneration: (actorRef?.generation ?? 0n).toString(),
          reply: reply?.data().toString('base64')
        };
        pending.phase = response.accepted ? 'admitted' : 'rejected';
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

  private async evaluateCommit(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    transferId: string,
    pending: ZLinkPendingRoutedActorTransfer
  ): Promise<Record<string, unknown>> {
    try {
      if (decoded.transferState === undefined || this.options.routedActorTransferProvider === undefined) {
        throw new Error('Remote actor transfer commit cannot materialize the target actor.');
      }
      const { actor, actorRef } = await this.options.routedActorTransferProvider(
        decoded.actorId,
        decoded.actorType,
        decoded.transferAdapterKey,
        decoded.transferState,
        decoded.actorEntryNodeRid,
        decoded.remoteBoundSessionTarget
      );
      const handoffResults = await this.options.commitTransferredActor?.(actor, decoded.handoffBacklog) ?? [];
      const commitReply = {
        accepted: true,
        actorNodeRid: String(actorRef.nodeRid),
        actorNodeRidHex: encodeRoutingIdHex(actorRef.nodeRid),
        actorId: actorRef.actorId,
        actorGeneration: actorRef.generation.toString(),
        handoffResults
      };
      pending.phase = 'committed';
      pending.commitReply = commitReply;
      this.scheduleExpiration(transferId, pending);
      return commitReply;
    } catch (error) {
      this.scheduleExpiration(transferId, pending);
      throw error;
    }
  }

  private runJoinCallback(
    decoded: ZLinkDecodedRemoteActorJoinRequest,
    request: Message
  ): Promise<ZLinkSpotActorJoinResult> {
    const target = this.options.getTarget();
    const joinPayload = wrapFrameworkPayloadMessage(request, this.options.messageSerializers);
    const actorRef = decoded.actorRef;
    if (actorRef === undefined) {
      // The commit half of the routed transfer materializes the Actor from this
      // ActorRef, so a request without one is rejected before admission runs
      // rather than after the application already accepted it.
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
      throw new Error(`Remote actor transfer '${decoded.transferId}' identity does not match its first request.`);
    }
  }

  private scheduleExpiration(transferId: string, pending: ZLinkPendingRoutedActorTransfer): void {
    if (pending.deadline !== undefined) clearTimeout(pending.deadline);
    pending.deadline = setTimeout(
      () => {
        if (this.pending.get(transferId) === pending) this.pending.delete(transferId);
      },
      this.options.pendingAdmissionTimeoutMs ?? 30_000
    );
    pending.deadline.unref();
  }

  private closeDecoded(decoded: ZLinkDecodedRemoteActorJoinRequest): void {
    decoded.request.close();
    decoded.actorCreateRequest?.close();
    decoded.transferState?.close();
  }

  private decodeRemoteActorJoinRequest(
    parts: readonly Message[],
    received: BackendReceived
  ): ZLinkDecodedRemoteActorJoinRequest | undefined {
    if (parts.length < 2 || parts[0].data().length === 0) {
      if (parts.length !== 1 || parts[0].data().length === 0) {
        return undefined;
      }
      try {
        const payload = JSON.parse(parts[0].data().toString()) as ZLinkRemoteActorJoinWirePayload;
        if (
          payload.packetName !== REMOTE_ACTOR_JOIN_PACKET ||
          !isRemoteActorJoinPayload(payload)
        ) {
          return undefined;
        }
        return decodeRemoteActorJoinPayload(
          payload,
          RuntimeMessage.from(Buffer.from(payload.request, 'base64')),
          received,
          true
        );
      } catch {
        return undefined;
      }
    }
    try {
      const envelope = decodeChannelEnvelope(parts);
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
        received,
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
          RuntimeMessage.from(Buffer.from(parts[1].data())),
          received,
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
