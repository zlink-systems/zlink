import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ActorRef } from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkSpotKind
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import {
  decodeActorRequestDeadlineUnixMs,
  decodeStreamHeader
} from '../streams/protocol';
import type { ZLinkActorRoutedJoinTransport } from './actor-routed-join-transport';
import { requestRoutedJsonReply } from './actor-routed-json-request';
import type { ZLinkRemoteBoundSessionTarget } from './actor-runtime-state';
import {
  encodeMessageFollowRemoteActorPacketRelayPayload,
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET
} from './actor-packet-relay-wire';
import {
  actorMessageFollowContext,
  actorMessageFollowPayloadChecksum,
  attachActorMessageFollowContext,
  advanceActorMessageFollowContext,
  createMessageFollowId,
  messageFollowOwnerFenceKey,
  messageFollowOwnerFencesEqual,
  ownerFence,
  verifyActorMessageFollowPayload,
  type ZLinkActorMessageFollowContext,
  type ZLinkActorMessageFollowOwnerFence
} from './actor-message-follow-context';

export const DEFAULT_MESSAGE_FOLLOW_DURATION_MS = 30_000;
const MAX_MESSAGE_FOLLOW_HOPS = 8;
const MAX_MESSAGE_FOLLOW_MESSAGES = 1024;
const MAX_MESSAGE_FOLLOW_BYTES = 16 * 1024 * 1024;
const RELOCATION_REPLY_RETENTION_MS = 24 * 60 * 60 * 1_000;

export interface ZLinkActorHandoffTarget {
  readonly routerChannelId: string;
  readonly targetNodeRid: string;
  readonly spotId: string;
  readonly spotKind?: ZLinkSpotKind;
}

export interface ZLinkActorHandoffPacket {
  readonly index: number;
  readonly header: string;
  readonly payload: string;
  readonly returnResponse: boolean;
  readonly source?: ZLinkActorHandoffRequestSource;
  readonly remoteBoundSessionTarget?: {
    readonly routerChannelId: string;
    readonly targetNodeRid: string;
    readonly spotId: string;
    readonly bindingGeneration?: string;
    readonly previousAuthorityOwnerGeneration?: string;
    readonly previousOwnerLeaseGeneration?: string;
    readonly acceptedHighWater?: string;
    readonly relocationSealId?: string;
    readonly acceptedJournalReference?: string;
    readonly acceptedJournalChecksumCrc32c?: number;
  };
  readonly fallbackActorRef?: {
    readonly actorId: string;
    readonly objectGeneration: string;
    readonly meshName: string;
    readonly nodeRid: string;
  };
  readonly messageFollowContext: ZLinkActorMessageFollowContext;
  readonly messageFollowOrigin?: ZLinkMessageFollowOrigin;
}

export interface ZLinkActorHandoffRequestSource {
  readonly ownerId: string;
  readonly ownerLeaseGeneration: string;
  readonly nodeRid: string;
  readonly nodeGeneration: string;
  readonly replyRouteId: string;
}

export type ZLinkActorHandoffTerminalAck =
  | 'terminalReceived'
  | 'alreadyTerminal'
  | 'notAcknowledged';

export interface ZLinkActorHandoffTerminalAcceptance {
  readonly status: ZLinkActorHandoffTerminalAck;
  readonly source?: ZLinkActorHandoffRequestSource;
}

export interface ZLinkActorHandoffResult {
  readonly index: number;
  readonly ok: boolean;
  readonly response?: unknown;
  readonly error?: string;
  readonly errorKind?: ZLinkFrameworkInternalErrorKind;
}

export type ZLinkActorHandoffDispatch = (
  parts: readonly Message[],
  returnResponse: boolean,
  remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
  fallbackActorRef?: ActorRef
) => Promise<unknown>;

interface PendingPacket {
  readonly packet: ZLinkActorHandoffPacket;
  readonly resolve?: (value: unknown) => void;
  readonly reject?: (reason: unknown) => void;
}

interface ReplyRoute {
  readonly actorId: string;
  readonly operationId: string;
  readonly source: ZLinkActorHandoffRequestSource;
  readonly resolve: (value: unknown) => void;
  readonly reject: (reason: unknown) => void;
  deadline?: ReturnType<typeof setTimeout>;
  targetNodeRid?: string;
  targetAuthorityOwnerGeneration?: bigint;
  delivered: boolean;
}

interface ActiveHandoff {
  readonly oldGeneration: bigint;
  readonly oldNodeRid?: string;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  phase: 'provisional' | 'relocating';
  readonly operationId?: string;
  replay?: ZLinkActorHandoffDispatch;
  readonly pending: PendingPacket[];
  nextIndex: number;
  snapshotIndex: number;
  pendingBytes: number;
}

interface MessageFollowRoute {
  readonly key: string;
  readonly oldGeneration: bigint;
  readonly oldNodeRid?: string;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  readonly targetOwner: ZLinkActorMessageFollowOwnerFence;
  readonly target: ZLinkSpotRouteTarget;
  readonly targetActorRef: ActorRef;
  readonly expiresAt: number;
  readonly deadline: ReturnType<typeof setTimeout>;
  tail: Promise<void>;
  queuedMessages: number;
  queuedBytes: number;
  readonly operations: Map<string, {
    readonly checksum: string;
    readonly request: boolean;
    readonly replyRouteId?: string;
    readonly result: Promise<unknown>;
  }>;
}

export interface ZLinkActorHandoffCoordinatorOptions {
  readonly routedTransport: ZLinkActorRoutedJoinTransport;
  readonly messageFollowDurationMs?: number;
  readonly requestTimeoutMs?: number;
  readonly onMarker?: (
    marker: string,
    actorId: string,
    index?: number,
    context?: ZLinkActorMessageFollowContext
  ) => void;
  readonly onRequestFrame?: (
    actorId: string,
    index: number,
    requestSeq: bigint | undefined,
    flags: number
  ) => void;
  readonly onMessageFollowRelayed?: (
    actorId: string,
    targetActorRef: ActorRef,
    context: ZLinkActorMessageFollowContext,
    origin: ZLinkMessageFollowOrigin,
    queuedMessages: number,
    queuedBytes: number
  ) => void | Promise<void>;
  readonly isStaleActorRef?: (actorId: string, actorRef?: ActorRef) => boolean;
  readonly isCurrentHandoffTarget?: (actorId: string, spotId: string) => boolean;
  readonly isCurrentActorRef?: (actorId: string, actorRef: ActorRef) => boolean;
  readonly requestSource?: () => {
    readonly ownerId: string;
    readonly ownerLeaseGeneration: bigint;
    readonly nodeRid: string;
    readonly nodeGeneration: bigint;
  } | undefined;
  readonly currentOwnerFence?: (
    actorId: string
  ) => ZLinkActorMessageFollowOwnerFence | undefined;
}

/** Owns packet ordering from relocation start through Message Follow removal. */
export class ZLinkActorHandoffCoordinator {
  private readonly active = new Map<string, ActiveHandoff>();
  private readonly messageFollowRoutes = new Map<string, MessageFollowRoute>();
  private readonly staleGenerations = new Map<string, Set<bigint>>();
  private readonly staleActorRefs = new Map<string, Set<string>>();
  private readonly messageFollowDurationMs: number;
  private nextReplyRouteId = 0n;
  private readonly replyRoutes = new Map<string, ReplyRoute>();

  constructor(private readonly options: ZLinkActorHandoffCoordinatorOptions) {
    this.messageFollowDurationMs = options.messageFollowDurationMs ?? DEFAULT_MESSAGE_FOLLOW_DURATION_MS;
  }

  begin(
    actorId: string,
    oldGeneration: bigint,
    oldNodeRid?: string,
    sourceAuthorityOwnerGeneration = 1n,
    sourceOwnerLeaseGeneration?: bigint
  ): void {
    if (this.active.has(actorId)) {
      throw new Error(`Actor '${actorId}' already has an active packet handoff.`);
    }
    const source = this.options.requestSource?.();
    const sourceOwner = ownerFence({
      ownerId: source?.ownerId ?? 'legacy-source-owner',
      ownerLeaseGeneration: sourceOwnerLeaseGeneration
        ?? source?.ownerLeaseGeneration
        ?? 1n,
      nodeRid: oldNodeRid ?? source?.nodeRid ?? 'legacy-source-node',
      nodeGeneration: source?.nodeGeneration ?? 1n,
      authorityOwnerGeneration: sourceAuthorityOwnerGeneration
    });
    this.active.set(actorId, {
      oldGeneration,
      oldNodeRid,
      sourceOwner,
      phase: 'relocating',
      pending: [],
      nextIndex: 0,
      snapshotIndex: -1,
      pendingBytes: 0
    });
  }

  beginProvisional(
    actorId: string,
    operationId: string,
    oldGeneration: bigint,
    oldNodeRid?: string,
    sourceAuthorityOwnerGeneration = 1n,
    sourceOwnerLeaseGeneration?: bigint
  ): void {
    if (this.active.has(actorId)) {
      throw new Error(`Actor '${actorId}' already has an active packet handoff.`);
    }
    const source = this.options.requestSource?.();
    const sourceOwner = ownerFence({
      ownerId: source?.ownerId ?? 'legacy-source-owner',
      ownerLeaseGeneration: sourceOwnerLeaseGeneration
        ?? source?.ownerLeaseGeneration
        ?? 1n,
      nodeRid: oldNodeRid ?? source?.nodeRid ?? 'legacy-source-node',
      nodeGeneration: source?.nodeGeneration ?? 1n,
      authorityOwnerGeneration: sourceAuthorityOwnerGeneration
    });
    this.active.set(actorId, {
      oldGeneration,
      oldNodeRid,
      sourceOwner,
      phase: 'provisional',
      operationId,
      pending: [],
      nextIndex: 0,
      snapshotIndex: -1,
      pendingBytes: 0
    });
  }

  isActive(actorId: string): boolean {
    return this.active.has(actorId);
  }

  isProvisional(actorId: string): boolean {
    return this.active.get(actorId)?.phase === 'provisional';
  }

  promoteProvisional(actorId: string, operationId: string): void {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) {
      throw new Error(`Actor '${actorId}' does not have a provisional packet handoff.`);
    }
    if (handoff.phase !== 'provisional' || handoff.operationId !== operationId) {
      throw new Error(`Actor '${actorId}' packet handoff belongs to another operation.`);
    }
    handoff.phase = 'relocating';
  }

  async releaseDeferred(
    actorId: string,
    operationId: string,
    dispatch?: ZLinkActorHandoffDispatch
  ): Promise<void> {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return;
    if (handoff.operationId !== operationId) {
      throw new Error(`Actor '${actorId}' packet handoff belongs to another operation.`);
    }
    this.active.delete(actorId);
    const replay = dispatch ?? handoff.replay;
    if (handoff.pending.length === 0) return;
    if (replay === undefined) {
      const error = new Error(`Actor '${actorId}' provisional packet handoff has no replay target.`);
      for (const pending of handoff.pending) {
        if (pending.packet.source !== undefined) {
          this.removeReplyRoute(pending.packet.source.replyRouteId);
        }
        pending.reject?.(error);
      }
      throw error;
    }
    const results = await replayActorHandoffBacklog(
      handoff.pending.map((pending) => pending.packet),
      replay
    );
    const byIndex = new Map(results.map((result) => [result.index, result]));
    for (const pending of handoff.pending) {
      if (pending.packet.source !== undefined) {
        this.removeReplyRoute(pending.packet.source.replyRouteId);
      }
      const result = byIndex.get(pending.packet.index);
      if (result?.ok === true) {
        pending.resolve?.(result.response);
      } else {
        pending.reject?.(new Error(result?.error ?? 'Actor provisional packet replay failed.'));
      }
    }
  }

  cancel(actorId: string): void {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return;
    this.active.delete(actorId);
    const error = new Error(`Actor '${actorId}' transfer was canceled.`);
    for (const pending of handoff.pending) {
      if (pending.packet.source !== undefined) {
        this.removeReplyRoute(pending.packet.source.replyRouteId);
      }
      pending.reject?.(error);
    }
  }

  capture(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    deadlineUnixMs?: number,
    messageFollowOrigin?: ZLinkMessageFollowOrigin,
    provisionalReplay?: ZLinkActorHandoffDispatch
  ): Promise<unknown> | undefined {
    const incomingContext = actorMessageFollowContext(fallbackActorRef);
    const originalDeadlineUnixMs = deadlineUnixMs
      ?? incomingContext?.deadlineUnixMs
      ?? packetDeadlineUnixMs(parts);
    const handoff = this.active.get(actorId);
    if (handoff !== undefined) {
      if (parts.length < 2) return Promise.resolve(undefined);
      if (handoff.phase === 'provisional' && handoff.replay === undefined) {
        handoff.replay = provisionalReplay;
      }
      const context = incomingContext ?? this.createInitialIngressContext(
        handoff.sourceOwner,
        handoff.oldGeneration,
        parts,
        returnResponse,
        originalDeadlineUnixMs
      );
      try {
        this.validateContext(actorId, context, parts, returnResponse, fallbackActorRef);
      } catch (error) {
        return Promise.reject(error);
      }
      const packet = encodePacket(
        handoff.nextIndex++,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        context,
        messageFollowOrigin
      );
      this.admitBounded(handoff.pending.length, handoff.pendingBytes, packet);
      handoff.pendingBytes += packetBytes(packet);
      this.options.onMarker?.('handoff_backlog', actorId, packet.index);
      if (returnResponse) {
        this.reportRequestFrame(actorId, packet);
      }
      if (!returnResponse) {
        handoff.pending.push({ packet });
        return Promise.resolve(undefined);
      }
      return new Promise((resolve, reject) => {
        const source = this.captureRequestSource(context.replyRouteId);
        const requestPacket = { ...packet, source };
        const route: ReplyRoute = {
          actorId,
          operationId: requestPacket.messageFollowContext.operationId,
          source,
          resolve,
          reject,
          delivered: false
        };
        route.deadline = setTimeout(() => {
          if (this.replyRoutes.get(source.replyRouteId) !== route) return;
          this.replyRoutes.delete(source.replyRouteId);
          if (!route.delivered) route.reject(new Error('Relocation reply route retention expired.'));
        }, RELOCATION_REPLY_RETENTION_MS);
        route.deadline.unref();
        this.replyRoutes.set(source.replyRouteId, route);
        handoff.pending.push({ packet: requestPacket, resolve, reject });
      });
    }

    const followRoute = this.findMessageFollowRoute(actorId, incomingContext);
    const staleGeneration = fallbackActorRef?.objectGeneration;
    if (
      fallbackActorRef !== undefined
      && this.options.isCurrentActorRef?.(actorId, fallbackActorRef) === true
    ) {
      if (incomingContext !== undefined) {
        this.validateContext(
          actorId,
          incomingContext,
          parts,
          returnResponse,
          fallbackActorRef
        );
        const currentFence = this.options.currentOwnerFence?.(actorId);
        if (currentFence !== undefined
            && !messageFollowOwnerFencesEqual(
              incomingContext.targetOwner,
              currentFence
            )) {
          this.options.onMarker?.('message_follow_rejected', actorId);
          return Promise.reject(actorLocationStale(actorId));
        }
      }
      return undefined;
    }
    if (
      incomingContext === undefined
      && fallbackActorRef !== undefined
      && this.hasMessageFollowRouteForStaleRef(actorId, fallbackActorRef)
    ) {
      this.options.onMarker?.('message_follow_rejected', actorId);
      return Promise.reject(actorLocationStale(actorId));
    }
    if (
      followRoute === undefined
      &&
      staleGeneration !== undefined
      && this.staleGenerations.get(actorId)?.has(staleGeneration) === true
      && fallbackActorRef !== undefined
      && this.isKnownStaleRef(actorId, fallbackActorRef)
    ) {
      this.options.onMarker?.('message_follow_expired', actorId);
      return Promise.reject(actorLocationStale(actorId));
    }
    if (followRoute === undefined) {
      if (fallbackActorRef !== undefined && this.isKnownStaleRef(actorId, fallbackActorRef)) {
        this.options.onMarker?.('message_follow_rejected', actorId);
        return Promise.reject(actorLocationStale(actorId));
      }
      return undefined;
    }
    if (Date.now() >= followRoute.expiresAt) {
      this.removeMessageFollowRoute(followRoute);
      this.options.onMarker?.('message_follow_rejected', actorId);
      return Promise.reject(actorLocationStale(actorId));
    }
    if (incomingContext === undefined) {
      this.options.onMarker?.('message_follow_rejected', actorId);
      return Promise.reject(actorLocationStale(actorId));
    }
    const context = incomingContext;
    try {
      this.validateContext(actorId, context, parts, returnResponse, fallbackActorRef);
    } catch (error) {
      return Promise.reject(error);
    }
    const packet = encodePacket(
      0,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      context
    );
    return this.enqueueMessageFollow(followRoute, actorId, packet);
  }

  snapshot(actorId: string): readonly ZLinkActorHandoffPacket[] {
    const handoff = this.requireActive(actorId);
    handoff.snapshotIndex = handoff.pending.length - 1;
    return handoff.pending.map((entry) => entry.packet);
  }

  snapshotCoreBacklog(actorId: string): readonly ZLinkActorHandoffPacket[] {
    const handoff = this.requireActive(actorId);
    let snapshotIndex = -1;
    while (
      snapshotIndex + 1 < handoff.pending.length
      && handoff.pending[snapshotIndex + 1].packet.returnResponse === false
    ) {
      snapshotIndex++;
    }
    handoff.snapshotIndex = snapshotIndex;
    return handoff.pending.slice(0, snapshotIndex + 1).map((entry) => entry.packet);
  }

  complete(
    actorId: string,
    target: ZLinkSpotRouteTarget,
    targetActorRef: ActorRef,
    results: readonly ZLinkActorHandoffResult[] = [],
    targetOwnerFence?: ZLinkActorMessageFollowOwnerFence
  ): void {
    const handoff = this.requireActive(actorId);
    this.active.delete(actorId);
    const byIndex = new Map(results.map((result) => [result.index, result]));
    for (let i = 0; i <= handoff.snapshotIndex; i++) {
      const pending = handoff.pending[i];
      if (pending.packet.source !== undefined) continue;
      const result = byIndex.get(pending.packet.index);
      if (result?.ok === false) {
        pending.reject?.(new Error(result.error ?? 'Actor handoff replay failed.'));
      } else if (result === undefined && pending.resolve !== undefined) {
        pending.reject?.(new Error(`Actor handoff reply '${pending.packet.index}' was not returned by the target.`));
      } else {
        pending.resolve?.(result?.response);
      }
    }

    const followRoute = this.installMessageFollowRoute(
      actorId,
      handoff.oldGeneration,
      handoff.oldNodeRid,
      handoff.sourceOwner,
      targetOwnerFence ?? ownerFence({
        ownerId: target.targetOwnerId ?? 'legacy-target-owner',
        ownerLeaseGeneration: target.ownerLeaseGeneration ?? 1n,
        nodeRid: String(targetActorRef.nodeRid),
        nodeGeneration: target.targetNodeGeneration ?? 1n,
        authorityOwnerGeneration: target.authorityOwnerGeneration ?? 1n
      }),
      target,
      targetActorRef
    );
    for (const pending of handoff.pending) {
      const source = pending.packet.source;
      if (source === undefined) continue;
      const route = this.replyRoutes.get(source.replyRouteId);
      if (route !== undefined) {
        route.targetNodeRid = String(target.targetNodeRid);
        route.targetAuthorityOwnerGeneration = target.authorityOwnerGeneration;
      }
    }
    for (let i = handoff.snapshotIndex + 1; i < handoff.pending.length; i++) {
      const pending = handoff.pending[i];
      if (pending.packet.source !== undefined) {
        this.removeReplyRoute(pending.packet.source.replyRouteId);
      }
      void this.enqueueMessageFollow(followRoute, actorId, pending.packet)
        .then(
          value => {
            pending.resolve?.(value);
          },
          error => {
            pending.reject?.(error);
          }
        );
    }
  }

  messageFollowCount(actorId?: string): number {
    return actorId === undefined
      ? this.messageFollowRoutes.size
      : [...this.messageFollowRoutes.values()]
          .filter(route => route.targetActorRef.actorId === actorId).length;
  }

  pendingCount(actorId: string): number {
    return this.active.get(actorId)?.pending.length ?? 0;
  }

  isKnownStale(actor: ActorRef): boolean {
    return this.messageFollowCount(actor.actorId) === 0
      && this.options.isCurrentActorRef?.(actor.actorId, actor) !== true
      && this.isKnownStaleRef(actor.actorId, actor);
  }

  recordStaleFailure(actorId: string): void {
    this.options.onMarker?.('message_follow_rejected', actorId);
  }

  acceptRelocatedTerminal(
    actorId: string,
    packet: ZLinkActorHandoffPacket,
    result: ZLinkActorHandoffResult,
    sourceNodeRid: string,
    targetAuthorityOwnerGeneration: bigint | undefined
  ): ZLinkActorHandoffTerminalAck {
    const source = packet.source;
    if (source === undefined || source.replyRouteId.length === 0) return 'notAcknowledged';
    return this.acceptRelocatedTerminalRelay(
      packet.messageFollowContext.operationId,
      source.replyRouteId,
      source,
      result,
      sourceNodeRid,
      targetAuthorityOwnerGeneration,
      actorId
    ).status;
  }

  acceptRelocatedTerminalRelay(
    operationId: string,
    replyRouteId: string,
    source: ZLinkActorHandoffRequestSource | undefined,
    result: ZLinkActorHandoffResult,
    sourceNodeRid: string,
    targetAuthorityOwnerGeneration: bigint | undefined,
    actorId?: string
  ): ZLinkActorHandoffTerminalAcceptance {
    if (replyRouteId.length === 0) return { status: 'notAcknowledged' };
    const currentSource = this.options.requestSource?.();
    const route = this.replyRoutes.get(replyRouteId);
    const exactSource = source ?? route?.source;
    if (currentSource === undefined || route === undefined || exactSource === undefined
      || currentSource.ownerId !== exactSource.ownerId
      || currentSource.ownerLeaseGeneration !== BigInt(exactSource.ownerLeaseGeneration)
      || currentSource.nodeRid !== exactSource.nodeRid
      || currentSource.nodeGeneration !== BigInt(exactSource.nodeGeneration)
      || (actorId !== undefined && route.actorId !== actorId)
      || route.operationId !== operationId
      || route.targetNodeRid !== sourceNodeRid
      || (targetAuthorityOwnerGeneration !== undefined
        && route.targetAuthorityOwnerGeneration !== targetAuthorityOwnerGeneration)) {
      return { status: 'notAcknowledged' };
    }
    if (route.delivered) return { status: 'alreadyTerminal', source: route.source };
    route.delivered = true;
    if (result.ok) {
      route.resolve(result.response);
    } else if (result.errorKind === ZLinkFrameworkInternalErrorKind.DeadlineExceeded) {
      route.reject(actorDeadlineExceeded(route.actorId));
    } else {
      route.reject(new Error(result.error ?? 'Actor handoff replay failed.'));
    }
    return { status: 'terminalReceived', source: route.source };
  }

  private captureRequestSource(replyRouteId?: string): ZLinkActorHandoffRequestSource {
    const source = this.options.requestSource?.();
    if (source === undefined || source.ownerId.length === 0
      || source.ownerLeaseGeneration <= 0n || source.nodeRid.length === 0
      || source.nodeGeneration <= 0n) {
      throw new Error('Actor handoff request requires an exact source owner fence.');
    }
    if (replyRouteId === undefined) {
      this.nextReplyRouteId += 1n;
      if (this.nextReplyRouteId <= 0n) {
        throw new Error('Actor handoff ReplyRouteId is exhausted.');
      }
      replyRouteId = this.nextReplyRouteId.toString();
    }
    return {
      ownerId: source.ownerId,
      ownerLeaseGeneration: source.ownerLeaseGeneration.toString(),
      nodeRid: source.nodeRid,
      nodeGeneration: source.nodeGeneration.toString(),
      replyRouteId
    };
  }

  private removeReplyRoute(replyRouteId: string): void {
    const route = this.replyRoutes.get(replyRouteId);
    if (route === undefined) return;
    if (route.deadline !== undefined) clearTimeout(route.deadline);
    this.replyRoutes.delete(replyRouteId);
  }

  private reportRequestFrame(actorId: string, packet: ZLinkActorHandoffPacket): void {
    try {
      const header = decodeStreamHeader(Buffer.from(packet.header, 'base64'));
      this.options.onRequestFrame?.(actorId, packet.index, header.requestSeq, header.flags);
    } catch {
      // Evidence collection must not change how a malformed packet is replayed and rejected.
    }
  }

  private requireActive(actorId: string): ActiveHandoff {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) throw new Error(`Actor '${actorId}' does not have an active packet handoff.`);
    return handoff;
  }

  private installMessageFollowRoute(
    actorId: string,
    oldGeneration: bigint,
    oldNodeRid: string | undefined,
    sourceOwner: ZLinkActorMessageFollowOwnerFence,
    targetOwner: ZLinkActorMessageFollowOwnerFence,
    target: ZLinkSpotRouteTarget,
    targetActorRef: ActorRef
  ): MessageFollowRoute {
    let stale = this.staleGenerations.get(actorId);
    if (stale === undefined) {
      stale = new Set();
      this.staleGenerations.set(actorId, stale);
    }
    stale.add(oldGeneration);
    if (oldNodeRid !== undefined) {
      let refs = this.staleActorRefs.get(actorId);
      if (refs === undefined) {
        refs = new Set();
        this.staleActorRefs.set(actorId, refs);
      }
      refs.add(actorRefKey(oldNodeRid, oldGeneration));
    }
    const key = messageFollowRouteKey(actorId, oldGeneration, sourceOwner);
    const previous = this.messageFollowRoutes.get(key);
    if (previous !== undefined) clearTimeout(previous.deadline);
    const expiresAt = Date.now() + this.messageFollowDurationMs;
    const entry = {
      key,
      oldGeneration,
      oldNodeRid,
      sourceOwner,
      targetOwner,
      target,
      targetActorRef,
      expiresAt,
      deadline: undefined as unknown as ReturnType<typeof setTimeout>,
      tail: Promise.resolve(),
      queuedMessages: 0,
      queuedBytes: 0,
      operations: new Map()
    };
    entry.deadline = setTimeout(
      () => this.removeMessageFollowRoute(entry),
      this.messageFollowDurationMs
    );
    entry.deadline.unref();
    this.messageFollowRoutes.set(key, entry);
    this.options.onMarker?.('message_follow_registered', actorId, this.messageFollowDurationMs);
    return entry;
  }

  private removeMessageFollowRoute(entry: MessageFollowRoute): void {
    if (this.messageFollowRoutes.get(entry.key) !== entry) return;
    clearTimeout(entry.deadline);
    this.messageFollowRoutes.delete(entry.key);
    this.options.onMarker?.(
      'message_follow_route_removed',
      entry.targetActorRef.actorId
    );
  }

  private findMessageFollowRoute(
    actorId: string,
    context: ZLinkActorMessageFollowContext | undefined
  ): MessageFollowRoute | undefined {
    if (context === undefined) return undefined;
    return this.messageFollowRoutes.get(messageFollowRouteKey(
      actorId,
      BigInt(context.objectGeneration),
      context.targetOwner
    ));
  }

  private hasMessageFollowRouteForStaleRef(actorId: string, actorRef: ActorRef): boolean {
    return [...this.messageFollowRoutes.values()].some(route =>
      route.targetActorRef.actorId === actorId
      && actorRef.objectGeneration === route.oldGeneration
      && (
        route.oldNodeRid === undefined
        || String(actorRef.nodeRid) === route.oldNodeRid
      )
    );
  }

  private createInitialIngressContext(
    sourceOwner: ZLinkActorMessageFollowOwnerFence,
    generation: bigint,
    parts: readonly Message[],
    request: boolean,
    deadlineUnixMs?: number
  ): ZLinkActorMessageFollowContext {
    const operationId = createMessageFollowId();
    return Object.freeze({
      operationId,
      objectGeneration: generation.toString(),
      sourceOwner,
      targetOwner: sourceOwner,
      deadlineUnixMs,
      correlationId: request ? createMessageFollowId() : undefined,
      replyRouteId: request ? createMessageFollowId() : undefined,
      request,
      hopCount: 0,
      visitedOwners: Object.freeze([messageFollowOwnerFenceKey(sourceOwner)]),
      payloadChecksumSha256: actorMessageFollowPayloadChecksum(parts)
    });
  }

  private validateContext(
    actorId: string,
    context: ZLinkActorMessageFollowContext,
    parts: readonly Message[],
    request: boolean,
    actorRef?: ActorRef
  ): void {
    if ((actorRef !== undefined
          && context.objectGeneration !== actorRef.objectGeneration.toString())
        || context.objectGeneration === '0'
        || context.request !== request) {
      throw actorLocationStale(actorId);
    }
    if (context.deadlineUnixMs !== undefined
        && Date.now() >= context.deadlineUnixMs) {
      throw actorDeadlineExceeded(actorId);
    }
    verifyActorMessageFollowPayload(context, parts);
  }

  private isKnownStaleRef(actorId: string, actor: ActorRef): boolean {
    const physicalRefs = this.staleActorRefs.get(actorId);
    if (physicalRefs !== undefined) {
      return physicalRefs.has(actorRefKey(String(actor.nodeRid), actor.objectGeneration));
    }
    return this.staleGenerations.get(actorId)?.has(actor.objectGeneration) === true
      && this.options.isStaleActorRef?.(actorId, actor) === true;
  }

  private enqueueMessageFollow(
    entry: MessageFollowRoute,
    actorId: string,
    packet: ZLinkActorHandoffPacket
  ): Promise<unknown> {
    const context = packet.messageFollowContext;
    const duplicate = entry.operations.get(context.operationId);
    if (duplicate !== undefined) {
      if (duplicate.checksum !== context.payloadChecksumSha256
          || duplicate.request !== context.request
          || duplicate.replyRouteId !== context.replyRouteId) {
        this.options.onMarker?.('message_follow_rejected', actorId);
        return Promise.reject(actorLocationStale(actorId));
      }
      return duplicate.result;
    }
    const bytes = packetBytes(packet);
    try {
      this.admitBounded(entry.queuedMessages, entry.queuedBytes, packet);
      if (entry.operations.size >= MAX_MESSAGE_FOLLOW_MESSAGES) {
        throw actorLocationStale(actorId);
      }
    } catch (error) {
      this.options.onMarker?.('message_follow_rejected', actorId);
      throw error;
    }
    if (context.hopCount >= MAX_MESSAGE_FOLLOW_HOPS) {
      this.options.onMarker?.('message_follow_rejected', actorId);
      return Promise.reject(actorLocationStale(actorId));
    }
    entry.queuedMessages++;
    entry.queuedBytes += bytes;
    let resolve!: (value: unknown) => void;
    let reject!: (reason: unknown) => void;
    const result = new Promise<unknown>((done, fail) => {
      resolve = done;
      reject = fail;
    });
    entry.operations.set(context.operationId, {
      checksum: context.payloadChecksumSha256,
      request: context.request,
      replyRouteId: context.replyRouteId,
      result
    });
    entry.tail = entry.tail.then(async () => {
      try {
        resolve(await this.relayMessageFollow(actorId, entry, packet));
      } catch (error) {
        this.options.onMarker?.('message_follow_rejected', actorId);
        reject(error);
      } finally {
        entry.queuedMessages--;
        entry.queuedBytes -= bytes;
      }
    });
    return result;
  }

  private admitBounded(messageCount: number, byteCount: number, packet: ZLinkActorHandoffPacket): void {
    if (
      messageCount >= MAX_MESSAGE_FOLLOW_MESSAGES
      || byteCount + packetBytes(packet) > MAX_MESSAGE_FOLLOW_BYTES
    ) {
      throw actorLocationStale('message-follow-bound');
    }
  }

  private async relayMessageFollow(
    actorId: string,
    entry: MessageFollowRoute,
    packet: ZLinkActorHandoffPacket
  ): Promise<unknown> {
    const { target, targetActorRef } = entry;
    const context = packet.messageFollowContext;
    let advanced: ZLinkActorMessageFollowContext;
    try {
      advanced = advanceActorMessageFollowContext(
        context,
        entry.sourceOwner,
        entry.targetOwner
      );
    } catch {
      throw actorLocationStale(actorId);
    }
    const payload = encodeMessageFollowRemoteActorPacketRelayPayload({
      actorId,
      routerChannelId: packet.remoteBoundSessionTarget?.routerChannelId,
      boundSessionTargetNodeRid: packet.remoteBoundSessionTarget?.targetNodeRid,
      boundSessionSpotId: packet.remoteBoundSessionTarget?.spotId,
      header: packet.header,
      payload: packet.payload,
      actorNodeRid: String(targetActorRef.nodeRid),
      actorGeneration: targetActorRef.objectGeneration.toString(),
      returnResponse: packet.returnResponse,
      messageFollowContext: advanced
    });
    if (!packet.returnResponse) {
      await this.options.routedTransport.sendToSpot(target, payload, {
        packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET
      });
      await this.messageFollowRelayed(actorId, entry, packet, advanced);
      return undefined;
    }
    const remainingMs = remainingRequestTime(
      actorId,
      packet.messageFollowContext.deadlineUnixMs
    );
    if (this.options.routedTransport.requestRawToSpot === undefined) {
      const reply = await awaitBeforeDeadline(
        this.options.routedTransport.requestToSpot<Record<string, unknown>>(target, payload, {
          packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
          timeoutMs: remainingMs ?? this.options.requestTimeoutMs
        }),
        actorId,
        remainingMs
      );
      if (reply.ok === false) {
        throw actorRelayError(actorId, reply.errorKind, reply.error);
      }
      await this.messageFollowRelayed(actorId, entry, packet, advanced);
      return reply.response;
    }
    const response = await awaitBeforeDeadline(
      requestRoutedJsonReply(
        this.options.routedTransport,
        target,
        payload,
        { timeoutMs: remainingMs ?? this.options.requestTimeoutMs },
        `Actor Message Follow raw request is not available for '${actorId}'.`,
        (parts) => {
          if (parts.length === 0) {
            throw new Error(`Actor Message Follow reply was empty for '${actorId}'.`);
          }
          const reply = JSON.parse(parts[0].getString('utf8')) as {
            readonly ok?: boolean;
            readonly response?: unknown;
            readonly error?: unknown;
            readonly errorKind?: unknown;
          };
          if (reply.ok === false) {
            throw actorRelayError(actorId, reply.errorKind, reply.error);
          }
          return reply.response;
        }
      ),
      actorId,
      remainingMs
    );
    await this.messageFollowRelayed(actorId, entry, packet, advanced);
    return response;
  }

  private async messageFollowRelayed(
    actorId: string,
    entry: MessageFollowRoute,
    packet: ZLinkActorHandoffPacket,
    context: ZLinkActorMessageFollowContext
  ): Promise<void> {
    this.options.onMarker?.('message_follow_relay', actorId, undefined, context);
    if (packet.messageFollowOrigin !== undefined) {
      await this.options.onMessageFollowRelayed?.(
        actorId,
        entry.targetActorRef,
        context,
        packet.messageFollowOrigin,
        entry.queuedMessages,
        entry.queuedBytes
      );
    }
  }
}

export function decodeHandoffPacket(packet: ZLinkActorHandoffPacket): {
  readonly parts: readonly Message[];
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly fallbackActorRef?: ActorRef;
} {
  return {
    parts: [
      RuntimeMessage.from(Buffer.from(packet.header, 'base64')) as Message,
      RuntimeMessage.from(Buffer.from(packet.payload, 'base64')) as Message
    ],
    remoteBoundSessionTarget: packet.remoteBoundSessionTarget === undefined
      ? undefined
      : {
          routerChannelId: packet.remoteBoundSessionTarget.routerChannelId,
          targetNodeRid: packet.remoteBoundSessionTarget.targetNodeRid,
          spotId: packet.remoteBoundSessionTarget.spotId,
          bindingGeneration: optionalBigInt(packet.remoteBoundSessionTarget.bindingGeneration),
          previousAuthorityOwnerGeneration:
            optionalBigInt(packet.remoteBoundSessionTarget.previousAuthorityOwnerGeneration),
          previousOwnerLeaseGeneration:
            optionalBigInt(packet.remoteBoundSessionTarget.previousOwnerLeaseGeneration),
          acceptedHighWater: optionalBigInt(packet.remoteBoundSessionTarget.acceptedHighWater),
          relocationSealId: packet.remoteBoundSessionTarget.relocationSealId,
          acceptedJournalReference: packet.remoteBoundSessionTarget.acceptedJournalReference,
          acceptedJournalChecksumCrc32c: packet.remoteBoundSessionTarget.acceptedJournalChecksumCrc32c
        },
    fallbackActorRef: packet.fallbackActorRef === undefined
      ? undefined
      : attachActorMessageFollowContext({
          actorId: packet.fallbackActorRef.actorId,
          objectGeneration: BigInt(packet.fallbackActorRef.objectGeneration),
          meshName: packet.fallbackActorRef.meshName,
          nodeRid: packet.fallbackActorRef.nodeRid
        }, packet.messageFollowContext)
  };
}

export async function replayActorHandoffBacklog(
  backlog: readonly ZLinkActorHandoffPacket[],
  dispatch: (
    parts: readonly Message[],
    returnResponse: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<unknown>,
  onEnqueued?: (index: number) => Promise<void> | void
): Promise<readonly ZLinkActorHandoffResult[]> {
  const results: ZLinkActorHandoffResult[] = [];
  for (const packet of backlog) {
    const decoded = decodeHandoffPacket(packet);
    try {
      if (
        packet.returnResponse
        && packet.messageFollowContext.deadlineUnixMs !== undefined
        && Date.now() >= packet.messageFollowContext.deadlineUnixMs
      ) {
        throw actorDeadlineExceeded(packet.fallbackActorRef?.actorId ?? 'accepted-handoff');
      }
      await onEnqueued?.(packet.index);
      const response = await dispatch(
        decoded.parts,
        packet.returnResponse,
        decoded.remoteBoundSessionTarget,
        decoded.fallbackActorRef
      );
      results.push({ index: packet.index, ok: true, response });
    } catch (error) {
      results.push({
        index: packet.index,
        ok: false,
        error: error instanceof Error ? error.message : String(error),
        errorKind: error instanceof ZLinkFrameworkException
          ? internalFrameworkErrorKind(error)
          : undefined
      });
    } finally {
      decoded.parts.forEach((part) => part.close());
    }
  }
  return results;
}

function encodePacket(
  index: number,
  parts: readonly Message[],
  returnResponse: boolean,
  remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
  fallbackActorRef?: ActorRef,
  messageFollowContext?: ZLinkActorMessageFollowContext,
  messageFollowOrigin?: ZLinkMessageFollowOrigin
): ZLinkActorHandoffPacket {
  if (messageFollowContext === undefined) {
    throw new Error('Actor handoff packet requires a Message Follow context.');
  }
  return {
    index,
    header: Buffer.from(parts[0].data()).toString('base64'),
    payload: Buffer.from(parts[1].data()).toString('base64'),
    returnResponse,
    messageFollowContext,
    ...(messageFollowOrigin === undefined ? {} : { messageFollowOrigin }),
    remoteBoundSessionTarget: remoteBoundSessionTarget === undefined
      ? undefined
      : {
          routerChannelId: remoteBoundSessionTarget.routerChannelId,
          targetNodeRid: String(remoteBoundSessionTarget.targetNodeRid),
          spotId: String(remoteBoundSessionTarget.spotId),
          bindingGeneration: remoteBoundSessionTarget.bindingGeneration?.toString(),
          previousAuthorityOwnerGeneration:
            remoteBoundSessionTarget.previousAuthorityOwnerGeneration?.toString(),
          previousOwnerLeaseGeneration:
            remoteBoundSessionTarget.previousOwnerLeaseGeneration?.toString(),
          acceptedHighWater: remoteBoundSessionTarget.acceptedHighWater?.toString(),
          relocationSealId: remoteBoundSessionTarget.relocationSealId,
          acceptedJournalReference: remoteBoundSessionTarget.acceptedJournalReference,
          acceptedJournalChecksumCrc32c: remoteBoundSessionTarget.acceptedJournalChecksumCrc32c
        },
    fallbackActorRef: fallbackActorRef === undefined
      ? undefined
      : {
          actorId: fallbackActorRef.actorId,
          objectGeneration: fallbackActorRef.objectGeneration.toString(),
          meshName: fallbackActorRef.meshName,
          nodeRid: String(fallbackActorRef.nodeRid)
        }
  };
}

function optionalBigInt(value: string | undefined): bigint | undefined {
  return value === undefined ? undefined : BigInt(value);
}

function packetBytes(packet: ZLinkActorHandoffPacket): number {
  return Buffer.byteLength(packet.header, 'base64') + Buffer.byteLength(packet.payload, 'base64');
}

function packetDeadlineUnixMs(parts: readonly Message[]): number | undefined {
  if (parts.length === 0) return undefined;
  try {
    return decodeActorRequestDeadlineUnixMs(parts[0].data());
  } catch {
    return undefined;
  }
}

function remainingRequestTime(actorId: string, deadlineUnixMs: number | undefined): number | undefined {
  if (deadlineUnixMs === undefined) return undefined;
  const remaining = deadlineUnixMs - Date.now();
  if (remaining <= 0) throw actorDeadlineExceeded(actorId);
  return Math.max(1, Math.ceil(remaining));
}

async function awaitBeforeDeadline<T>(
  operation: Promise<T>,
  actorId: string,
  remainingMs: number | undefined
): Promise<T> {
  if (remainingMs === undefined) return await operation;
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      operation,
      new Promise<never>((_resolve, reject) => {
        timer = setTimeout(() => reject(actorDeadlineExceeded(actorId)), remainingMs);
      })
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
}

function messageFollowRouteKey(
  actorId: string,
  objectGeneration: bigint,
  sourceOwner: ZLinkActorMessageFollowOwnerFence
): string {
  return `${actorId}\u0000${objectGeneration}\u0000${sourceOwner.authorityOwnerGeneration}`;
}

function actorRefKey(nodeRid: string, generation: bigint): string {
  return `${nodeRid}\u0000${generation}`;
}

function actorLocationStale(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorLocationStale,
    `Actor route '${actorId}' is stale after the Message Follow duration.`
  );
}

function actorDeadlineExceeded(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
    `Actor request '${actorId}' exceeded its original deadline during Message Follow.`
  );
}

function actorRelayError(
  actorId: string,
  errorKind: unknown,
  error: unknown
): ZLinkFrameworkException {
  const kind = Object.values(ZLinkFrameworkInternalErrorKind)
    .includes(errorKind as ZLinkFrameworkInternalErrorKind)
    ? errorKind as ZLinkFrameworkInternalErrorKind
    : ZLinkFrameworkInternalErrorKind.RequestFailed;
  return createInternalFrameworkException(
    kind,
    String(error ?? `Actor Message Follow relay failed for '${actorId}'.`),
    kind === ZLinkFrameworkInternalErrorKind.ActorMoving
      || kind === ZLinkFrameworkInternalErrorKind.RouteNotConnected
  );
}
