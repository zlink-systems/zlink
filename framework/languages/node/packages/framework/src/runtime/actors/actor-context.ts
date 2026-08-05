import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  RoutingId,
  SpotId,
  ZLinkActor,
  ZLinkActorContext,
  ZLinkActorJoinCompletion,
  ZLinkActorJoinEntrySpotCall,
  ZLinkActorJoinSpotCall,
  ZLinkBoundSession
} from '../../contracts';
import {
  ZLinkEncodedPayload,
  ZLinkFrameworkException,
  ZLinkMessage
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { type ZLinkMessageSerializer } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import {
  encodeFrameworkPayloadMessage
} from '../messaging/payload-codec';
import {
  ZLinkActorRuntimeState,
  toFrameworkActorRef
} from './actor-runtime-state';
import type {
  ZLinkActorBoundSessionFactory,
  ZLinkActorJoinCoordinator
} from './actor-runtime-contracts';
import type { ZLinkActorManagerOptions } from './actor-runtime-contracts';
import {
  ZLINK_ACTOR_LIFECYCLE_SNAPSHOT,
  type ZLinkActorLifecycleSnapshotSource
} from './actor-lifecycle-snapshot';
import { randomBytes } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { deferActorJoin } from './actor-join-deferred-scope';
import { captureZLinkSpotSerialTurn } from '../execution';

export const ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME = Symbol('zlink.actor.join-entry-spot-runtime');

export class DefaultZLinkActorContext implements ZLinkActorContext {
  readonly boundSession: ZLinkBoundSession;
  private resolvedMeshName: string | undefined;

  constructor(
    private readonly state: ZLinkActorRuntimeState,
    private readonly joinCoordinator: ZLinkActorJoinCoordinator | undefined,
    boundSessionFactory: ZLinkActorBoundSessionFactory | undefined,
    private readonly messageSerializers: ReadonlyMap<string, ZLinkMessageSerializer> | undefined,
    private readonly meshNameProvider: ZLinkActorManagerOptions['actorMeshNameProvider']
  ) {
    this.boundSession = boundSessionFactory?.(state.actorId) ?? new UnboundZLinkSession();
  }

  get meshName(): string {
    // The membership resolves once per actor. A getter that re-entered the
    // application provider on every read would make an observable side effect
    // out of reading identity.
    if (this.resolvedMeshName !== undefined) {
      return this.resolvedMeshName;
    }
    const remembered = this.state.meshName;
    if (remembered !== undefined) {
      this.resolvedMeshName = remembered;
      return remembered;
    }
    const actorType = this.state.actorType;
    const meshName = actorType === undefined ? undefined : this.meshNameProvider?.(actorType);
    if (meshName === undefined) {
      throw new ZLinkConfigurationException(
        `Actor '${this.state.actorId}' does not belong to a registered RouteMesh.`
      );
    }
    this.resolvedMeshName = meshName;
    return meshName;
  }

  get actorId(): string {
    return this.state.actorId;
  }

  get objectGeneration(): bigint {
    return this.state.nativeActorRef?.generation ?? 1n;
  }

  get spotId(): SpotId | undefined {
    return this.state.spotId;
  }

  [ZLINK_ACTOR_LIFECYCLE_SNAPSHOT](): ZLinkActorLifecycleSnapshotSource {
    const nativeActorRef = this.state.nativeActorRef;
    const actorType = this.state.actorType;
    if (nativeActorRef === undefined || actorType === undefined) {
      throw new ZLinkConfigurationException(
        `Actor '${this.state.actorId}' lifecycle identity is not initialized.`
      );
    }
    return {
      actorRef: toFrameworkActorRef(nativeActorRef, this.meshName),
      actorType,
      membershipEpoch: this.state.spotMembershipEpoch
    };
  }

  joinSpot(spotId: SpotId, request?: unknown): ZLinkActorJoinSpotCall {
    return new DefaultZLinkActorJoinSpotCall(
      this.state,
      this.requireActor(),
      this.requireJoinCoordinator(),
      spotId,
      request,
      this.messageSerializers
    );
  }

  joinEntrySpot(request?: unknown): ZLinkActorJoinEntrySpotCall {
    return new DefaultZLinkActorJoinEntrySpotCall(
      this.state,
      this.requireActor(),
      this.requireJoinCoordinator(),
      request,
      this.messageSerializers
    );
  }

  async [ZLINK_ACTOR_JOIN_ENTRY_SPOT_RUNTIME](
    nodeRid: RoutingId | undefined,
    request: unknown,
    signal?: AbortSignal
  ): Promise<boolean> {
    const requestMessage = encodeJoinRequest(request, this.messageSerializers);
    try {
      return (await this.requireJoinCoordinator().joinEntrySpot(
        this.requireActor(),
        this.state,
        nodeRid,
        requestMessage,
        undefined,
        signal
      )).accepted;
    } finally {
      requestMessage.close();
    }
  }

  private requireActor(): ZLinkActor {
    if (this.state.actor === undefined) {
      throw new ZLinkConfigurationException('Actor context is not bound to an actor.');
    }
    return this.state.actor;
  }

  private requireJoinCoordinator(): ZLinkActorJoinCoordinator {
    if (this.joinCoordinator === undefined) {
      throw new ZLinkConfigurationException('Actor join runtime is not started.');
    }
    return this.joinCoordinator;
  }
}

class DefaultZLinkActorJoinSpotCall implements ZLinkActorJoinSpotCall {
  private timeoutMs = 5_000;
  private deferred = false;
  private readonly turn = captureZLinkSpotSerialTurn();

  constructor(
    private readonly state: ZLinkActorRuntimeState,
    private readonly actor: ZLinkActor,
    private readonly coordinator: ZLinkActorJoinCoordinator,
    private readonly spotId: SpotId,
    private readonly request: unknown,
    private readonly messageSerializers: ReadonlyMap<string, ZLinkMessageSerializer> | undefined
  ) {
  }

  timeout(timeoutMs: number): this {
    this.timeoutMs = validateJoinTimeout(timeoutMs);
    return this;
  }

  defer(): void {
    if (this.deferred) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
        'Actor join call was already deferred.'
      );
    }
    this.deferred = true;
    const deadline = performance.now() + this.timeoutMs;
    const sourceNodeRid = this.state.nativeActorRef?.nodeRid;
    const requestMessage = encodeJoinRequest(this.request, this.messageSerializers);
    try {
      claimDeferredJoin(this.state);
    } catch (error) {
      requestMessage.close();
      throw error;
    }
    const operationId = createJoinOperationId();
    let discarded = false;
    let prepared = false;
    let pendingJoin: Promise<import('./actor-runtime-contracts').ZLinkActorJoinRuntimeResult<Message>> | undefined;
    const discard = async (): Promise<void> => {
      if (discarded) return;
      discarded = true;
      this.state.endDeferredJoin();
      try {
        await this.coordinator.abortDeferredJoin?.(this.actor, this.state, operationId);
      } finally {
        requestMessage.close();
      }
    };
    try {
      deferActorJoin({
        requestBytes: requestMessage.data().byteLength,
        prepare: async () => {
          if (prepared) return;
          prepared = true;
          this.state.endDeferredJoin();
          try {
            // The provisional ingress fence is installed only after the
            // handler terminal. Target admission starts before reply encoding,
            // while completion and finalization remain in execute().
            this.coordinator.beginDeferredJoin?.(this.actor, this.state, operationId);
            pendingJoin = this.coordinator.joinSpot(
              this.actor,
              this.state,
              this.spotId,
              requestMessage,
              remainingJoinTimeout(deadline),
              undefined,
              operationId
            );
            if (this.turn?.yieldAllowed !== true) {
              await pendingJoin.catch(() => undefined);
            }
          } catch (error) {
            pendingJoin = Promise.reject(error);
            void pendingJoin.catch(() => undefined);
          }
        },
        discard,
        execute: async () => {
        let result: import('./actor-runtime-contracts').ZLinkActorJoinRuntimeResult<Message>;
        try {
          const pending = pendingJoin ?? this.coordinator.joinSpot(
            this.actor,
            this.state,
            this.spotId,
            requestMessage,
            remainingJoinTimeout(deadline),
            undefined,
            operationId
          );
          result = this.turn?.yieldAllowed === true
            ? await this.turn.yieldPromise(pending)
            : await pending;
        } catch (error) {
          this.state.endDeferredJoin();
          await notifyJoinFailure(this.actor, operationId, error);
          await this.coordinator.abortDeferredJoin?.(this.actor, this.state, operationId)
            .catch(() => undefined);
          return;
        } finally {
          requestMessage.close();
        }
        if (
          sourceNodeRid === undefined
          || result.actor === undefined
          || String(sourceNodeRid) === String(result.actor.nodeRid)
        ) {
          await notifyJoinCompletion(
            this.actor, operationId, result, this.messageSerializers);
        } else {
          result.reply?.close();
        }
        await result.finalizeDeferredJoin?.();
        }
      });
    } catch (error) {
      void discard().catch(() => undefined);
      throw error;
    }
  }
}

class DefaultZLinkActorJoinEntrySpotCall implements ZLinkActorJoinEntrySpotCall {
  private timeoutMs = 5_000;
  private deferred = false;
  private readonly turn = captureZLinkSpotSerialTurn();

  constructor(
    private readonly state: ZLinkActorRuntimeState,
    private readonly actor: ZLinkActor,
    private readonly coordinator: ZLinkActorJoinCoordinator,
    private readonly request: unknown,
    private readonly messageSerializers: ReadonlyMap<string, ZLinkMessageSerializer> | undefined
  ) {
  }

  timeout(timeoutMs: number): this {
    this.timeoutMs = validateJoinTimeout(timeoutMs);
    return this;
  }

  defer(): void {
    if (this.deferred) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.AlreadySubmitted,
        'Actor join call was already deferred.'
      );
    }
    this.deferred = true;
    const deadline = performance.now() + this.timeoutMs;
    const sourceNodeRid = this.state.nativeActorRef?.nodeRid;
    const requestMessage = encodeJoinRequest(this.request, this.messageSerializers);
    try {
      claimDeferredJoin(this.state);
    } catch (error) {
      requestMessage.close();
      throw error;
    }
    const operationId = createJoinOperationId();
    let discarded = false;
    let prepared = false;
    let pendingJoin: Promise<import('./actor-runtime-contracts').ZLinkActorJoinRuntimeResult<Message>> | undefined;
    const discard = async (): Promise<void> => {
      if (discarded) return;
      discarded = true;
      this.state.endDeferredJoin();
      try {
        await this.coordinator.abortDeferredJoin?.(this.actor, this.state, operationId);
      } finally {
        requestMessage.close();
      }
    };
    try {
      deferActorJoin({
        requestBytes: requestMessage.data().byteLength,
        prepare: async () => {
          if (prepared) return;
          prepared = true;
          this.state.endDeferredJoin();
          try {
            this.coordinator.beginDeferredJoin?.(this.actor, this.state, operationId);
            pendingJoin = this.coordinator.joinEntrySpot(
              this.actor,
              this.state,
              undefined,
              requestMessage,
              remainingJoinTimeout(deadline),
              undefined,
              operationId
            );
            if (this.turn?.yieldAllowed !== true) {
              await pendingJoin.catch(() => undefined);
            }
          } catch (error) {
            pendingJoin = Promise.reject(error);
            void pendingJoin.catch(() => undefined);
          }
        },
        discard,
        execute: async () => {
        let result: import('./actor-runtime-contracts').ZLinkActorJoinRuntimeResult<Message>;
        try {
          const pending = pendingJoin ?? this.coordinator.joinEntrySpot(
            this.actor,
            this.state,
            undefined,
            requestMessage,
            remainingJoinTimeout(deadline),
            undefined,
            operationId
          );
          result = this.turn?.yieldAllowed === true
            ? await this.turn.yieldPromise(pending)
            : await pending;
        } catch (error) {
          this.state.endDeferredJoin();
          await notifyJoinFailure(this.actor, operationId, error);
          await this.coordinator.abortDeferredJoin?.(this.actor, this.state, operationId)
            .catch(() => undefined);
          return;
        } finally {
          requestMessage.close();
        }
        if (
          sourceNodeRid === undefined
          || result.actor === undefined
          || String(sourceNodeRid) === String(result.actor.nodeRid)
        ) {
          await notifyJoinCompletion(
            this.actor, operationId, result, this.messageSerializers);
        } else {
          result.reply?.close();
        }
        await result.finalizeDeferredJoin?.();
        }
      });
    } catch (error) {
      void discard().catch(() => undefined);
      throw error;
    }
  }
}

class UnboundZLinkSession implements ZLinkBoundSession {
  send(): never {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      'Actor session is not bound.',
      true
    );
  }

  async disconnect(): Promise<void> {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
      'Actor session is not bound.',
      true
    );
  }
}

function encodeJoinRequest(
  request: unknown,
  serializers: ReadonlyMap<string, ZLinkMessageSerializer> | undefined
): Message {
  return request === undefined
    ? RuntimeMessage.from(Buffer.alloc(0))
    : encodeFrameworkPayloadMessage(request, serializers);
}

function validateJoinTimeout(timeoutMs: number): number {
  const rounded = Math.ceil(timeoutMs);
  if (!Number.isFinite(timeoutMs) || rounded < 1 || rounded > 2_147_483_647) {
    throw new ZLinkConfigurationException(
      'Actor join timeout must be a finite value from 1 through 2147483647 milliseconds.'
    );
  }
  return rounded;
}

function claimDeferredJoin(state: ZLinkActorRuntimeState): void {
  if (!state.tryBeginDeferredJoin()) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.ActorMoving,
      `Actor '${state.actorId}' already has a pending membership transition.`,
      true
    );
  }
}

function remainingJoinTimeout(deadline: number): number {
  const remaining = deadline - performance.now();
  if (remaining <= 0) {
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      'Deferred Actor join deadline elapsed before activation.',
      true
    );
  }
  return Math.ceil(remaining);
}

function createJoinOperationId(): { readonly high: bigint; readonly low: bigint } {
  const bytes = randomBytes(16);
  return {
    high: bytes.readBigUInt64BE(0),
    low: bytes.readBigUInt64BE(8)
  };
}

async function notifyJoinCompletion(
  actor: ZLinkActor,
  operationId: { readonly high: bigint; readonly low: bigint },
  result: import('./actor-runtime-contracts').ZLinkActorJoinRuntimeResult<Message>,
  _messageSerializers: ReadonlyMap<string, ZLinkMessageSerializer> | undefined
): Promise<void> {
  // The completion preserves the encoded reply without exposing serializer selection.
  const reply = result.reply === undefined
    ? undefined
    : ZLinkMessage.fromEncoded(
      ZLinkEncodedPayload.from(result.reply.data())
    );
  result.reply?.close();
  // `reply`는 계약상 선택 항목이라 없을 때는 key 자체를 만들지 않는다.
  const replyField = reply === undefined ? {} : { reply };
  const completion: ZLinkActorJoinCompletion = result.accepted
    ? {
        status: 'accepted',
        operationId,
        actor: result.actor!,
        ...replyField
      }
    : {
        status: 'rejected',
        operationId,
        ...replyField
      };
  await actor.onJoinCompleted?.(completion);
}

async function notifyJoinFailure(
  actor: ZLinkActor,
  operationId: { readonly high: bigint; readonly low: bigint },
  error: unknown
): Promise<void> {
  console.error(`actor join failure actor=${actor.context.actorId}`, error);
  const frameworkError = error instanceof ZLinkFrameworkException
    ? error
    : createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestFailed,
        'Deferred actor join failed.',
        false,
        error
      );
  await actor.onJoinCompleted?.({
    status: 'failed',
    operationId,
    kind: frameworkError.kind
  });
}
