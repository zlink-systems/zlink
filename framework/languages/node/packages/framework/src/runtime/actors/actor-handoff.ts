import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ActorRef, RoutingId } from '../../contracts';
import {
  ZLinkFrameworkException
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import {
  MessageFollowSuppressionRegistry,
  type MessageFollowSuppressionFence
} from '../foundation/message-follow-suppression-registry';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import { encodeRoutingIdStorageHex, routingIdsEqual } from '../routing-id';
import { releaseApplicationJobPermitForDurableHandoff } from '../application-jobs/application-job-queue-scope';
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
  messageFollowOwnerNodeRid,
  ownerFence,
  verifyActorMessageFollowPayload,
  type ZLinkActorMessageFollowContext,
  type ZLinkActorMessageFollowOwnerFence
} from './actor-message-follow-context';

export const DEFAULT_MESSAGE_FOLLOW_DURATION_MS = 30_000;
const MAX_MESSAGE_FOLLOW_HOPS = 8;
const RELOCATION_REPLY_RETENTION_MS = 24 * 60 * 60 * 1_000;

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
    readonly relocationSealId?: string;
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

export type ZLinkActorHandoffReplayAdmission = <T>(
  operation: () => Promise<T>
) => Promise<T>;

export interface ZLinkActorHandoffPreparedReplayAdmission {
  run<T>(operation: () => Promise<T>): Promise<T>;
  cancel(): void;
}

export type ZLinkActorHandoffReplayPreparation = (
  signal: AbortSignal
) => Promise<ZLinkActorHandoffPreparedReplayAdmission>;

export interface ZLinkActorHandoffPrefixAdmission {
  /** Completes after every child callback reaches its real terminal. */
  readonly terminal: Promise<void>;
}

export interface ZLinkActorHandoffPrefixRecord {
  readonly drain: (dispatch: ZLinkActorHandoffDispatch) => Promise<void>;
  readonly preparation?: {
    prepare(signal: AbortSignal): Promise<void>;
    cancel(): void;
  };
  readonly payloadBytes: number;
}

/**
 * Atomically installs one non-executing durable prefix in an Actor FIFO.
 * Invoking the function is the queue-admission linearization point; a failure
 * to admit must throw synchronously.
 *
 * @internal
 */
export type ZLinkActorHandoffPrefixQueue = (
  records: readonly ZLinkActorHandoffPrefixRecord[]
) => ZLinkActorHandoffPrefixAdmission;

interface PendingPacket {
  readonly packet: ZLinkActorHandoffPacket;
  readonly resolve?: (value: unknown) => void;
  readonly reject?: (reason: unknown) => void;
}

interface ActivePrefixRelease {
  readonly queue: ZLinkActorHandoffPrefixQueue;
  readonly preparation?: ZLinkActorHandoffReplayPreparation;
  readonly start: Promise<void>;
  startOpen: boolean;
  cursor: number;
  inFlight: number;
  readonly errors: unknown[];
  readonly terminal: Promise<void>;
  resolve(): void;
  reject(reason: unknown): void;
}

interface ReplySourceEvidence {
  readonly meshName: string;
  readonly objectGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly nodeRid: string;
  readonly nodeRidHex: string;
  readonly nodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
}

interface ReplyRoute {
  readonly actorId: string;
  readonly operationId: string;
  readonly source: ZLinkActorHandoffRequestSource;
  readonly sourceEvidence: ReplySourceEvidence;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  readonly resolve: (value: unknown) => void;
  readonly reject: (reason: unknown) => void;
  deadline?: ReturnType<typeof setTimeout>;
  targetNodeRid?: RoutingId;
  targetAuthorityOwnerGeneration?: bigint;
  delivered: boolean;
}

interface ActiveHandoff {
  readonly oldGeneration: bigint;
  readonly oldNodeRid?: string;
  readonly oldNodeRidHex?: string;
  readonly sourceEvidence: ReplySourceEvidence;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  phase: 'provisional' | 'releasing' | 'relocating';
  readonly operationId?: string;
  replay?: ZLinkActorHandoffDispatch;
  connectionBoundSealed: boolean;
  readonly pending: PendingPacket[];
  nextIndex: number;
  snapshotIndex: number;
  pendingBytes: number;
  releaseTask?: Promise<void>;
  prefixRelease?: ActivePrefixRelease;
}

interface MessageFollowRoute {
  readonly key: string;
  readonly oldGeneration: bigint;
  readonly oldNodeRid?: string;
  readonly oldNodeRidHex?: string;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  readonly targetOwner: ZLinkActorMessageFollowOwnerFence;
  readonly target: ZLinkSpotRouteTarget;
  readonly targetActorRef: ActorRef;
  readonly suppressionFence: MessageFollowSuppressionFence;
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
  ) => boolean | Promise<boolean>;
  readonly isStaleActorRef?: (actorId: string, actorRef?: ActorRef) => boolean;
  readonly isCurrentHandoffTarget?: (actorId: string, spotId: string) => boolean;
  readonly isCurrentActorRef?: (actorId: string, actorRef: ActorRef) => boolean;
  readonly requestSource: (actorId: string) => {
    readonly meshName: string;
    readonly objectGeneration: bigint;
    readonly ownerId: string;
    readonly ownerLeaseGeneration: bigint;
    readonly nodeRid: string;
    readonly nodeRidHex?: string;
    readonly nodeGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
  };
  readonly validateReplySource: (source: ReplySourceEvidence) => boolean;
  readonly currentOwnerFence?: (
    actorId: string
  ) => ZLinkActorMessageFollowOwnerFence | undefined;
}

function requireExactTargetOwnerFence(
  actorId: string,
  fence: ZLinkActorMessageFollowOwnerFence | undefined
): ZLinkActorMessageFollowOwnerFence {
  if (fence === undefined) {
    throw new Error(`Actor '${actorId}' handoff requires a committed target owner fence.`);
  }
  try {
    return ownerFence(fence);
  } catch {
    throw new Error(`Actor '${actorId}' handoff requires a committed target owner fence.`);
  }
}

/** Owns packet ordering from relocation start through Message Follow removal. */
export class ZLinkActorHandoffCoordinator {
  private readonly active = new Map<string, ActiveHandoff>();
  private readonly messageFollowRoutes = new Map<string, MessageFollowRoute>();
  private readonly messageFollowSuppression = new MessageFollowSuppressionRegistry();
  // Relocation preserves ObjectGeneration, so (nodeRid, objectGeneration) alone
  // also matches a tenure that later RETURNS to a previously-visited node. The
  // stale-tenure identity therefore keeps the departing tenure's
  // authorityOwnerGeneration: a ref is stale only while its authority fence is
  // not strictly newer than the recorded departure. Entries live exactly as
  // long as their Message Follow route and are pruned with it.
  private readonly staleGenerations = new Map<string, Map<bigint, bigint>>();
  private readonly staleActorRefs = new Map<string, Map<string, bigint>>();
  private readonly messageFollowDurationMs: number;
  private nextReplyRouteId = 0n;
  private readonly replyRoutes = new Map<string, ReplyRoute>();

  constructor(private readonly options: ZLinkActorHandoffCoordinatorOptions) {
    this.messageFollowDurationMs = options.messageFollowDurationMs ?? DEFAULT_MESSAGE_FOLLOW_DURATION_MS;
  }

  begin(
    actorId: string,
    oldGeneration: bigint
  ): void {
    this.startHandoff(actorId, oldGeneration, { phase: 'relocating' });
  }

  beginProvisional(
    actorId: string,
    operationId: string,
    oldGeneration: bigint
  ): void {
    this.startHandoff(
      actorId,
      oldGeneration,
      { phase: 'provisional', operationId }
    );
  }

  private startHandoff(
    actorId: string,
    oldGeneration: bigint,
    transition: { readonly phase: 'relocating' } | {
      readonly phase: 'provisional';
      readonly operationId: string;
    }
  ): void {
    if (this.active.has(actorId)) {
      throw new Error(`Actor '${actorId}' already has an active packet handoff.`);
    }
    if (oldGeneration <= 0n) {
      throw new Error(`Actor '${actorId}' handoff requires a positive source ObjectGeneration.`);
    }
    const source = this.options.requestSource(actorId);
    if (source.meshName.length === 0 || source.ownerId.length === 0
      || source.ownerLeaseGeneration <= 0n
      || source.nodeRid.length === 0 || source.nodeGeneration <= 0n
      || source.authorityOwnerGeneration <= 0n) {
      throw new Error(`Actor '${actorId}' handoff requires an exact source owner fence.`);
    }
    if (source.objectGeneration !== oldGeneration) {
      throw new Error(
        `Actor '${actorId}' handoff source ObjectGeneration changed from ${oldGeneration} to ${source.objectGeneration}.`
      );
    }
    const sourceOwner = ownerFence({
      ownerId: source.ownerId,
      ownerLeaseGeneration: source.ownerLeaseGeneration,
      nodeRid: source.nodeRid,
      nodeRidHex: source.nodeRidHex,
      nodeGeneration: source.nodeGeneration,
      authorityOwnerGeneration: source.authorityOwnerGeneration
    });
    const sourceEvidence = Object.freeze({
      meshName: source.meshName,
      objectGeneration: source.objectGeneration,
      ownerId: source.ownerId,
      ownerLeaseGeneration: source.ownerLeaseGeneration,
      nodeRid: sourceOwner.nodeRid,
      nodeRidHex: sourceOwner.nodeRidHex
        ?? encodeRoutingIdStorageHex(sourceOwner.nodeRid),
      nodeGeneration: source.nodeGeneration,
      authorityOwnerGeneration: source.authorityOwnerGeneration
    });
    this.active.set(actorId, {
      oldGeneration,
      oldNodeRid: sourceOwner.nodeRid,
      oldNodeRidHex: sourceEvidence.nodeRidHex,
      sourceEvidence,
      sourceOwner,
      phase: transition.phase,
      ...(transition.phase === 'provisional' ? { operationId: transition.operationId } : {}),
      connectionBoundSealed: false,
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

  sealConnectionBoundIngress(actorId: string): void {
    const handoff = this.active.get(actorId);
    if (handoff !== undefined) handoff.connectionBoundSealed = true;
  }

  activeSourceOwner(actorId: string): ZLinkActorMessageFollowOwnerFence | undefined {
    return this.active.get(actorId)?.sourceOwner;
  }

  async releaseDeferred(
    actorId: string,
    operationId: string,
    dispatch?: ZLinkActorHandoffDispatch,
    admission?: ZLinkActorHandoffReplayAdmission
  ): Promise<void> {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return;
    if (handoff.operationId !== operationId) {
      throw new Error(`Actor '${actorId}' packet handoff belongs to another operation.`);
    }
    await this.releaseHandoff(
      actorId,
      handoff,
      dispatch ?? handoff.replay,
      admission
    );
  }

  async releaseCanceled(
    actorId: string,
    dispatch: ZLinkActorHandoffDispatch,
    admission?: ZLinkActorHandoffReplayAdmission
  ): Promise<void> {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return;
    await this.releaseHandoff(actorId, handoff, dispatch, admission);
  }

  /**
   * Restores a deferred hold as one durable FIFO prefix. The returned
   * admission means every held record is owned by the source/target Actor
   * queue, not that any handler has run.
   *
   * @internal
   */
  admitDeferredPrefix(
    actorId: string,
    operationId: string,
    queue: ZLinkActorHandoffPrefixQueue,
    preparation?: ZLinkActorHandoffReplayPreparation,
    start: Promise<void> = Promise.resolve()
  ): ZLinkActorHandoffPrefixAdmission {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return { terminal: Promise.resolve() };
    if (handoff.operationId !== operationId) {
      throw new Error(`Actor '${actorId}' packet handoff belongs to another operation.`);
    }
    return this.admitHandoffPrefix(actorId, handoff, queue, preparation, start);
  }

  /** @internal Restores a canceled relocation hold as one Actor FIFO prefix. */
  admitCanceledPrefix(
    actorId: string,
    queue: ZLinkActorHandoffPrefixQueue,
    preparation?: ZLinkActorHandoffReplayPreparation,
    start: Promise<void> = Promise.resolve()
  ): ZLinkActorHandoffPrefixAdmission {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return { terminal: Promise.resolve() };
    return this.admitHandoffPrefix(actorId, handoff, queue, preparation, start);
  }

  private admitHandoffPrefix(
    actorId: string,
    handoff: ActiveHandoff,
    queue: ZLinkActorHandoffPrefixQueue,
    preparation: ZLinkActorHandoffReplayPreparation | undefined,
    start: Promise<void>
  ): ZLinkActorHandoffPrefixAdmission {
    if (handoff.releaseTask !== undefined) {
      return { terminal: handoff.releaseTask };
    }
    handoff.phase = 'releasing';
    let resolveTerminal!: () => void;
    let rejectTerminal!: (reason: unknown) => void;
    const terminal = new Promise<void>((resolve, reject) => {
      resolveTerminal = resolve;
      rejectTerminal = reject;
    });
    const release: ActivePrefixRelease = {
      queue,
      preparation,
      start,
      startOpen: false,
      cursor: 0,
      inFlight: 0,
      errors: [],
      terminal,
      resolve: resolveTerminal,
      reject: rejectTerminal
    };
    handoff.prefixRelease = release;
    handoff.releaseTask = terminal;
    void start.then(
      () => {
        release.startOpen = true;
        this.finishPrefixReleaseIfIdle(actorId, handoff, release);
      },
      error => {
        release.startOpen = true;
        if (handoff.pending.length === 0) release.errors.push(error);
        this.finishPrefixReleaseIfIdle(actorId, handoff, release);
      }
    );
    try {
      this.admitPendingPrefix(actorId, handoff, release);
    } catch (error) {
      // The synchronous caller receives the admission error directly. Observe
      // the shared terminal as well so this path cannot create an unhandled
      // rejection before the relocation rollback reports the original error.
      void terminal.catch(() => undefined);
      this.finishPrefixReleaseIfIdle(actorId, handoff, release);
      throw error;
    }
    return { terminal };
  }

  private admitPendingPrefix(
    actorId: string,
    handoff: ActiveHandoff,
    release: ActivePrefixRelease
  ): void {
    if (release.cursor >= handoff.pending.length) return;
    const begin = release.cursor;
    const end = handoff.pending.length;
    const pending = handoff.pending.slice(begin, end);
    let admitted: ZLinkActorHandoffPrefixAdmission;
    try {
      admitted = release.queue(pending.map(record =>
        this.createPrefixRecord(record, release)));
    } catch (error) {
      this.failUnqueuedPrefix(handoff, release, error);
      throw error;
    }
    release.cursor = end;
    release.inFlight += 1;
    void admitted.terminal.then(
      () => this.completePrefixBatch(actorId, handoff, release),
      error => {
        release.errors.push(error);
        this.completePrefixBatch(actorId, handoff, release);
      }
    );
  }

  private createPrefixRecord(
    pending: PendingPacket,
    release: ActivePrefixRelease
  ): ZLinkActorHandoffPrefixRecord {
    let prepared: ZLinkActorHandoffPreparedReplayAdmission | undefined;
    const cancel = (): void => {
      prepared?.cancel();
      prepared = undefined;
    };
    return {
      payloadBytes: packetBytes(pending.packet),
      preparation: {
        prepare: async signal => {
          try {
            await waitForHandoffReplayStart(release.start, signal);
            if (release.preparation !== undefined) {
              const next = await release.preparation(signal);
              if (signal.aborted) {
                next.cancel();
                throw signal.reason;
              }
              prepared = next;
            }
          } catch (error) {
            if (!signal.aborted) this.failReleasedPending(pending, error);
            throw error;
          }
        },
        cancel
      },
      drain: async replay => {
        const admission = prepared === undefined
          ? undefined
          : <T>(operation: () => Promise<T>) => prepared!.run(operation);
        try {
          await this.replayReleasedPending(pending, replay, admission);
        } finally {
          cancel();
        }
      }
    };
  }

  private completePrefixBatch(
    actorId: string,
    handoff: ActiveHandoff,
    release: ActivePrefixRelease
  ): void {
    release.inFlight -= 1;
    if (release.inFlight === 0 && release.cursor < handoff.pending.length) {
      try {
        this.admitPendingPrefix(actorId, handoff, release);
      } catch {
        // admitPendingPrefix recorded the exact error and rejected every
        // request that never reached the Actor FIFO.
      }
    }
    this.finishPrefixReleaseIfIdle(actorId, handoff, release);
  }

  private finishPrefixReleaseIfIdle(
    actorId: string,
    handoff: ActiveHandoff,
    release: ActivePrefixRelease
  ): void {
    if (!release.startOpen
        || release.inFlight !== 0
        || release.cursor !== handoff.pending.length) return;
    if (handoff.prefixRelease === release) handoff.prefixRelease = undefined;
    if (this.active.get(actorId) === handoff) this.active.delete(actorId);
    if (release.errors.length === 0) {
      release.resolve();
    } else if (release.errors.length === 1) {
      release.reject(release.errors[0]);
    } else {
      release.reject(new AggregateError(
        release.errors,
        `Actor '${actorId}' durable prefix replay failed.`
      ));
    }
  }

  private failUnqueuedPrefix(
    handoff: ActiveHandoff,
    release: ActivePrefixRelease,
    error: unknown
  ): void {
    release.errors.push(error);
    for (let index = release.cursor; index < handoff.pending.length; index += 1) {
      this.failReleasedPending(handoff.pending[index]!, error);
    }
    release.cursor = handoff.pending.length;
  }

  private async releaseHandoff(
    actorId: string,
    handoff: ActiveHandoff,
    replay: ZLinkActorHandoffDispatch | undefined,
    admission?: ZLinkActorHandoffReplayAdmission
  ): Promise<void> {
    if (handoff.releaseTask !== undefined) {
      await handoff.releaseTask;
      return;
    }
    // Keep the hold installed while each record obtains a fresh application
    // permit. Arrivals appended during an awaited replay remain behind the
    // frozen prefix, and the final active-map removal is atomic with observing
    // the drained tail. A shared task makes duplicate release callbacks join
    // the same drain instead of replaying a record twice.
    handoff.phase = 'releasing';
    const releaseTask = Promise.resolve().then(
      () => this.replayReleasedBacklog(actorId, handoff, replay, admission)
    );
    handoff.releaseTask = releaseTask;
    await releaseTask;
  }

  private async replayReleasedBacklog(
    actorId: string,
    handoff: ActiveHandoff,
    replay: ZLinkActorHandoffDispatch | undefined,
    admission?: ZLinkActorHandoffReplayAdmission
  ): Promise<void> {
    try {
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
      let cursor = 0;
      const oneWayErrors: unknown[] = [];
      while (cursor < handoff.pending.length) {
        const pending = handoff.pending[cursor++];
        try {
          await this.replayReleasedPending(pending, replay, admission);
        } catch (error) {
          oneWayErrors.push(error);
        }
      }
      if (oneWayErrors.length === 1) throw oneWayErrors[0];
      if (oneWayErrors.length > 1) {
        throw new AggregateError(oneWayErrors, `Actor '${actorId}' handoff replay failed.`);
      }
    } finally {
      if (this.active.get(actorId) === handoff) {
        this.active.delete(actorId);
      }
    }
  }

  private async replayReleasedPending(
    pending: PendingPacket,
    replay: ZLinkActorHandoffDispatch,
    admission?: ZLinkActorHandoffReplayAdmission
  ): Promise<void> {
    const results = await replayActorHandoffBacklog(
      [pending.packet],
      (parts, returnResponse, remoteBoundSessionTarget, fallbackActorRef) => {
        const operation = () => replay(
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef
        );
        return admission === undefined ? operation() : admission(operation);
      }
    );
    if (pending.packet.source !== undefined) {
      this.removeReplyRoute(pending.packet.source.replyRouteId);
    }
    const result = results[0]!;
    if (result.ok === true) {
      pending.resolve?.(result.response);
      return;
    }
    const error = result.errorKind === undefined
      ? new Error(result.error ?? 'Actor provisional packet replay failed.')
      : createInternalFrameworkException(
          result.errorKind,
          result.error ?? 'Actor provisional packet replay failed.'
        );
    if (pending.reject !== undefined) {
      pending.reject(error);
      return;
    }
    throw error;
  }

  private failReleasedPending(pending: PendingPacket, error: unknown): void {
    if (pending.packet.source !== undefined) {
      this.removeReplyRoute(pending.packet.source.replyRouteId);
    }
    pending.reject?.(error);
  }

  cancel(actorId: string): void {
    const handoff = this.active.get(actorId);
    if (handoff === undefined) return;
    if (handoff.phase === 'releasing') return;
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
      if (handoff.connectionBoundSealed && !returnResponse && remoteBoundSessionTarget !== undefined) {
        // The Session route seal already fixed the accepted high water. A
        // send whose lifetime is only the Session connection can no longer
        // drain here and must never enter the durable frozen journal, so the
        // Session owner keeps redelivery ownership of it.
        return Promise.reject(actorMoving(actorId));
      }
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
      handoff.pendingBytes += packetBytes(packet);
      this.options.onMarker?.('handoff_backlog', actorId, packet.index);
      if (returnResponse) {
        this.reportRequestFrame(actorId, packet);
      }
      if (!returnResponse) {
        handoff.pending.push({ packet });
        // The durable handoff now owns this record. Return the ingress permit
        // before waiting for a later replay, which must acquire its own fresh
        // application-job permit.
        releaseApplicationJobPermitForDurableHandoff();
        if (handoff.prefixRelease !== undefined) {
          try {
            this.admitPendingPrefix(actorId, handoff, handoff.prefixRelease);
          } catch (error) {
            this.finishPrefixReleaseIfIdle(actorId, handoff, handoff.prefixRelease);
            return Promise.reject(error);
          }
        }
        return Promise.resolve(undefined);
      }
      const result = new Promise((resolve, reject) => {
        const source = this.captureRequestSource(
          handoff.sourceEvidence,
          context.replyRouteId
        );
        const requestPacket = { ...packet, source };
        const route: ReplyRoute = {
          actorId,
          operationId: requestPacket.messageFollowContext.operationId,
          source,
          sourceEvidence: handoff.sourceEvidence,
          sourceOwner: handoff.sourceOwner,
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
      releaseApplicationJobPermitForDurableHandoff();
      if (handoff.prefixRelease !== undefined) {
        try {
          this.admitPendingPrefix(actorId, handoff, handoff.prefixRelease);
        } catch {
          // failUnqueuedPrefix rejected this request with the exact queue
          // admission error and finish below closes the shared release.
          this.finishPrefixReleaseIfIdle(actorId, handoff, handoff.prefixRelease);
        }
      }
      return result;
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
          // A relocation round trip (A→B→A) keeps the ObjectGeneration, so a
          // context resolved against a departed tenure still addresses this
          // node while the current tenure holds strictly newer authority
          // fences. The departed fence keeps its exact-fence Message Follow
          // chain: relay through it instead of a terminal stale verdict.
          if (followRoute !== undefined) {
            if (Date.now() >= followRoute.expiresAt) {
              this.removeMessageFollowRoute(followRoute);
              this.options.onMarker?.('message_follow_rejected', actorId);
              return Promise.reject(actorLocationStale(actorId));
            }
            const packet = encodePacket(
              0,
              parts,
              returnResponse,
              remoteBoundSessionTarget,
              fallbackActorRef,
              incomingContext,
              messageFollowOrigin
            );
            return this.enqueueMessageFollow(followRoute, actorId, packet);
          }
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
      context,
      messageFollowOrigin
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

  /**
   * Freezes the post-capture ingress segment immediately before the caller
   * appends the one-way cutover on the same relocation lane. Records returned
   * here become part of the transferred prefix and are therefore excluded
   * from the later Message Follow tail.
   */
  takeRelocationRelay(actorId: string): readonly ZLinkActorHandoffPacket[] {
    const handoff = this.requireActive(actorId);
    const first = handoff.snapshotIndex + 1;
    handoff.snapshotIndex = handoff.pending.length - 1;
    return handoff.pending.slice(first).map(entry => entry.packet);
  }

  complete(
    actorId: string,
    target: ZLinkSpotRouteTarget,
    targetActorRef: ActorRef,
    results: readonly ZLinkActorHandoffResult[],
    targetOwnerFence: ZLinkActorMessageFollowOwnerFence
  ): void {
    const handoff = this.requireActive(actorId);
    const committedTargetOwner = requireExactTargetOwnerFence(actorId, targetOwnerFence);
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
      handoff.oldNodeRidHex,
      handoff.sourceOwner,
      committedTargetOwner,
      target,
      targetActorRef
    );
    for (const pending of handoff.pending) {
      const source = pending.packet.source;
      if (source === undefined) continue;
      const route = this.replyRoutes.get(source.replyRouteId);
      if (route !== undefined) {
        route.targetNodeRid = target.targetNodeRid;
        route.targetAuthorityOwnerGeneration = BigInt(
          committedTargetOwner.authorityOwnerGeneration
        );
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
    sourceNodeRid: RoutingId,
    targetAuthorityOwnerGeneration: bigint | undefined
  ): ZLinkActorHandoffTerminalAck {
    const source = packet.source;
    if (source === undefined || source.replyRouteId.length === 0) return 'notAcknowledged';
    const route = this.replyRoutes.get(source.replyRouteId);
    if (route === undefined
      || packet.messageFollowContext.objectGeneration
        !== route.sourceEvidence.objectGeneration.toString()
      || !messageFollowOwnerFencesEqual(
        packet.messageFollowContext.targetOwner,
        route.sourceOwner
      )) {
      return 'notAcknowledged';
    }
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
    sourceNodeRid: RoutingId,
    targetAuthorityOwnerGeneration: bigint | undefined,
    actorId?: string
  ): ZLinkActorHandoffTerminalAcceptance {
    if (replyRouteId.length === 0) return { status: 'notAcknowledged' };
    const route = this.replyRoutes.get(replyRouteId);
    if (route === undefined) return { status: 'notAcknowledged' };
    const exactSource = source ?? route.source;
    if (!requestSourcesEqual(route.source, exactSource)
      || (actorId !== undefined && route.actorId !== actorId)
      || route.operationId !== operationId
      || route.targetNodeRid === undefined
      || !routingIdsEqual(route.targetNodeRid, sourceNodeRid)
      || route.targetAuthorityOwnerGeneration === undefined
      || (targetAuthorityOwnerGeneration !== undefined
        && route.targetAuthorityOwnerGeneration !== targetAuthorityOwnerGeneration)) {
      return { status: 'notAcknowledged' };
    }
    try {
      if (this.options.validateReplySource(route.sourceEvidence) !== true) {
        return { status: 'notAcknowledged' };
      }
    } catch {
      return { status: 'notAcknowledged' };
    }
    if (route.delivered) {
      return { status: 'alreadyTerminal', source: terminalRequestSource(route) };
    }
    route.delivered = true;
    if (result.ok) {
      route.resolve(result.response);
    } else if (result.errorKind === ZLinkFrameworkInternalErrorKind.DeadlineExceeded) {
      route.reject(actorDeadlineExceeded(route.actorId));
    } else {
      route.reject(new Error(result.error ?? 'Actor handoff replay failed.'));
    }
    return { status: 'terminalReceived', source: terminalRequestSource(route) };
  }

  private captureRequestSource(
    source: ReplySourceEvidence,
    replyRouteId?: string
  ): ZLinkActorHandoffRequestSource {
    if (replyRouteId === undefined) {
      this.nextReplyRouteId += 1n;
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
    oldNodeRidHex: string | undefined,
    sourceOwner: ZLinkActorMessageFollowOwnerFence,
    targetOwner: ZLinkActorMessageFollowOwnerFence,
    target: ZLinkSpotRouteTarget,
    targetActorRef: ActorRef
  ): MessageFollowRoute {
    const departedAuthorityOwnerGeneration = BigInt(sourceOwner.authorityOwnerGeneration);
    let stale = this.staleGenerations.get(actorId);
    if (stale === undefined) {
      stale = new Map();
      this.staleGenerations.set(actorId, stale);
    }
    const recordedGeneration = stale.get(oldGeneration);
    if (recordedGeneration === undefined || recordedGeneration < departedAuthorityOwnerGeneration) {
      stale.set(oldGeneration, departedAuthorityOwnerGeneration);
    }
    if (oldNodeRid !== undefined) {
      let refs = this.staleActorRefs.get(actorId);
      if (refs === undefined) {
        refs = new Map();
        this.staleActorRefs.set(actorId, refs);
      }
      const exactRefKey = actorRefEvidenceKey(
        oldNodeRid,
        oldNodeRidHex,
        oldGeneration
      );
      const recordedRef = refs.get(exactRefKey);
      if (recordedRef === undefined || recordedRef < departedAuthorityOwnerGeneration) {
        refs.set(exactRefKey, departedAuthorityOwnerGeneration);
      }
    }
    const key = messageFollowRouteKey(actorId, oldGeneration, sourceOwner);
    const previous = this.messageFollowRoutes.get(key);
    if (previous !== undefined) clearTimeout(previous.deadline);
    const suppressionFence = actorMessageFollowSuppressionFence(
      actorId,
      oldGeneration,
      sourceOwner,
      targetOwner
    );
    const expiresAt = Date.now() + this.messageFollowDurationMs;
    const entry = {
      key,
      oldGeneration,
      oldNodeRid,
      oldNodeRidHex,
      sourceOwner,
      targetOwner,
      target,
      targetActorRef,
      suppressionFence,
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
    if (previous === undefined) {
      this.messageFollowSuppression.retainRoute(suppressionFence);
    } else {
      this.messageFollowSuppression.replaceRoute(
        previous.suppressionFence,
        suppressionFence
      );
    }
    this.messageFollowRoutes.set(key, entry);
    this.options.onMarker?.('message_follow_registered', actorId, this.messageFollowDurationMs);
    return entry;
  }

  private removeMessageFollowRoute(entry: MessageFollowRoute): void {
    if (this.messageFollowRoutes.get(entry.key) !== entry) return;
    clearTimeout(entry.deadline);
    this.messageFollowRoutes.delete(entry.key);
    this.messageFollowSuppression.expireRoute(entry.suppressionFence);
    this.pruneStaleTenureRecords(entry);
    this.options.onMarker?.(
      'message_follow_route_removed',
      entry.targetActorRef.actorId
    );
  }

  /**
   * Stale-tenure records must outlive only the Message Follow window of the
   * departure that wrote them. A later departure of the same
   * (nodeRid, objectGeneration) re-records a newer authority generation, so a
   * record is removed only while it still belongs to this route's departure.
   */
  private pruneStaleTenureRecords(entry: MessageFollowRoute): void {
    const actorId = entry.targetActorRef.actorId;
    const departedAuthorityOwnerGeneration = BigInt(entry.sourceOwner.authorityOwnerGeneration);
    const stale = this.staleGenerations.get(actorId);
    if (stale !== undefined) {
      const recordedGeneration = stale.get(entry.oldGeneration);
      if (recordedGeneration !== undefined
          && recordedGeneration <= departedAuthorityOwnerGeneration) {
        stale.delete(entry.oldGeneration);
      }
      if (stale.size === 0) this.staleGenerations.delete(actorId);
    }
    if (entry.oldNodeRid === undefined) return;
    const refs = this.staleActorRefs.get(actorId);
    if (refs === undefined) return;
    const refKey = actorRefEvidenceKey(
      entry.oldNodeRid,
      entry.oldNodeRidHex,
      entry.oldGeneration
    );
    const recordedRef = refs.get(refKey);
    if (recordedRef !== undefined && recordedRef <= departedAuthorityOwnerGeneration) {
      refs.delete(refKey);
    }
    if (refs.size === 0) this.staleActorRefs.delete(actorId);
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
        || routingIdsEqual(
          actorRef.nodeRid,
          messageFollowOwnerNodeRid(route.sourceOwner)
        )
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
    if (actorRef !== undefined
        && context.objectGeneration !== actorRef.objectGeneration.toString()) {
      throw actorGenerationStale(actorId);
    }
    if (context.objectGeneration === '0' || context.request !== request) {
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
      const departedAuthorityOwnerGeneration = physicalRefs.get(
        actorRefKey(actor.nodeRid, actor.objectGeneration)
      );
      return departedAuthorityOwnerGeneration !== undefined
        && !this.isNewerTenureRef(actorId, actor, departedAuthorityOwnerGeneration);
    }
    const departedAuthorityOwnerGeneration =
      this.staleGenerations.get(actorId)?.get(actor.objectGeneration);
    return departedAuthorityOwnerGeneration !== undefined
      && !this.isNewerTenureRef(actorId, actor, departedAuthorityOwnerGeneration)
      && this.options.isStaleActorRef?.(actorId, actor) === true;
  }

  /**
   * The stale record captures the departing tenure's full fence. A ref whose
   * authority fence is strictly newer than the recorded departure addresses a
   * returning tenure (A→B→A keeps the ObjectGeneration), never the departed
   * one, so it must not match the stale record.
   */
  private isNewerTenureRef(
    actorId: string,
    actor: ActorRef,
    departedAuthorityOwnerGeneration: bigint
  ): boolean {
    const tenure = this.tenureAuthorityOwnerGeneration(actorId, actor);
    return tenure !== undefined && tenure > departedAuthorityOwnerGeneration;
  }

  private tenureAuthorityOwnerGeneration(
    actorId: string,
    actor: ActorRef
  ): bigint | undefined {
    const context = actorMessageFollowContext(actor);
    if (context !== undefined) {
      return BigInt(context.targetOwner.authorityOwnerGeneration);
    }
    const current = this.options.currentOwnerFence?.(actorId);
    if (current !== undefined && current.nodeRid === String(actor.nodeRid)) {
      return BigInt(current.authorityOwnerGeneration);
    }
    return undefined;
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
      actorNodeRidHex: encodeRoutingIdStorageHex(targetActorRef.nodeRid),
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
      const claim = this.messageFollowSuppression.begin(entry.suppressionFence);
      if (claim === undefined) return;
      let accepted = false;
      try {
        accepted = await this.options.onMessageFollowRelayed?.(
          actorId,
          entry.targetActorRef,
          context,
          packet.messageFollowOrigin,
          entry.queuedMessages,
          entry.queuedBytes
        ) === true;
      } catch {
        accepted = false;
      }
      if (accepted) {
        this.messageFollowSuppression.markSent(claim);
      } else {
        this.messageFollowSuppression.abort(claim);
      }
    }
  }
}

function actorMessageFollowSuppressionFence(
  actorId: string,
  objectGeneration: bigint,
  source: ZLinkActorMessageFollowOwnerFence,
  target: ZLinkActorMessageFollowOwnerFence
): MessageFollowSuppressionFence {
  return Object.freeze({
    objectKind: 'actor',
    logicalObjectId: actorId,
    objectGeneration: objectGeneration.toString(),
    sourceNodeRid: source.nodeRid,
    ...(source.nodeRidHex === undefined ? {} : { sourceNodeRidHex: source.nodeRidHex }),
    sourceNodeGeneration: source.nodeGeneration,
    sourceAuthorityOwnerGeneration: source.authorityOwnerGeneration,
    sourceOwnerLeaseGeneration: source.ownerLeaseGeneration,
    targetNodeRid: target.nodeRid,
    ...(target.nodeRidHex === undefined ? {} : { targetNodeRidHex: target.nodeRidHex }),
    targetNodeGeneration: target.nodeGeneration,
    targetAuthorityOwnerGeneration: target.authorityOwnerGeneration,
    targetOwnerLeaseGeneration: target.ownerLeaseGeneration
  });
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
          relocationSealId: packet.remoteBoundSessionTarget.relocationSealId
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
          relocationSealId: remoteBoundSessionTarget.relocationSealId
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

async function waitForHandoffReplayStart(
  start: Promise<void>,
  signal: AbortSignal
): Promise<void> {
  if (signal.aborted) throw signal.reason;
  let abort!: () => void;
  const aborted = new Promise<never>((_resolve, reject) => {
    abort = () => reject(signal.reason);
    signal.addEventListener('abort', abort, { once: true });
  });
  try {
    await Promise.race([start, aborted]);
  } finally {
    signal.removeEventListener('abort', abort);
  }
}

function messageFollowRouteKey(
  actorId: string,
  objectGeneration: bigint,
  sourceOwner: ZLinkActorMessageFollowOwnerFence
): string {
  return `${actorId}\u0000${objectGeneration}\u0000${messageFollowOwnerFenceKey(sourceOwner)}`;
}

function actorRefKey(nodeRid: ActorRef['nodeRid'], generation: bigint): string {
  return `${encodeRoutingIdStorageHex(nodeRid)}\u0000${generation}`;
}

function actorRefEvidenceKey(
  nodeRid: string,
  nodeRidHex: string | undefined,
  generation: bigint
): string {
  return `${nodeRidHex ?? encodeRoutingIdStorageHex(nodeRid)}\u0000${generation}`;
}

function requestSourcesEqual(
  expected: ZLinkActorHandoffRequestSource,
  actual: ZLinkActorHandoffRequestSource
): boolean {
  return expected.replyRouteId === actual.replyRouteId
    && expected.ownerId === actual.ownerId
    && expected.ownerLeaseGeneration === actual.ownerLeaseGeneration
    && expected.nodeRid === actual.nodeRid
    && expected.nodeGeneration === actual.nodeGeneration
    && /^[1-9][0-9]*$/.test(actual.ownerLeaseGeneration)
    && /^[1-9][0-9]*$/.test(actual.nodeGeneration);
}

function terminalRequestSource(route: ReplyRoute): ZLinkActorHandoffRequestSource {
  return Object.freeze({
    ...route.source,
    // The durable packet keeps its legacy text field. The local ACK path can
    // restore the exact binary RID from the captured owner fence without
    // changing that packet wire shape.
    nodeRid: messageFollowOwnerNodeRid(route.sourceOwner)
  });
}

function actorLocationStale(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorLocationStale,
    `Actor route '${actorId}' is stale after the Message Follow duration.`
  );
}

function actorGenerationStale(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorGenerationStale,
    `Actor route '${actorId}' carries a different object generation.`
  );
}

function actorDeadlineExceeded(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
    `Actor request '${actorId}' exceeded its original deadline during Message Follow.`
  );
}

function actorMoving(actorId: string): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.ActorMoving,
    `Actor '${actorId}' bound-session send arrived after its Session route seal.`,
    true
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
