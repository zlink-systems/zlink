import { randomBytes } from 'node:crypto';
import {
  Message,
  RoutingId as BindingRoutingId,
  RequestResult,
  SubmitResult,
  type RequestResult as RequestResultValue,
  type StreamSocket,
  type SubmitResult as SubmitResultValue
} from '@zlink-systems/zlink';
import {
  isZLinkBackendResultError,
  type ZLinkBackendMessageLike as MessageLike
} from '../runtime-values';
import type {
  MeshOperationId,
  MeshPeerEntry,
  MeshPublisher,
  ReadyBatch,
  ReadyRecord,
  ReceiveBatch,
  ReceiveRecord,
  ServiceSpot,
  StreamSessionService
} from '../../foundation/service-runtime-contracts';
import {
  OperationKind,
  ReadyDomain,
  ReadyOwnerKind,
  ReceiveKind
} from '../../foundation/service-runtime-contracts';
import {
  OperationCancelledError,
  OperationTimeoutError
} from '../../foundation/operation-registry';
import {
  RawServiceMeshRuntime,
  type RawServiceRequestResult
} from '../../foundation/raw-service-mesh-runtime';
import {
  decodeApplicationPayloadView,
  encodeMultipartApplicationPayload,
  ServiceWireProtocolError
} from '../../foundation/service-wire-m6a-codec';
import {
  SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE,
  SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME
} from '../../foundation/service-wire-constants.generated';
import {
  ServiceStatefulRuntime,
  statefulMailboxData,
  type ServiceAsyncInstanceActivationAuthority,
  type ServiceInstanceApplicationLifecycle,
  type ServicePendingInstanceActivation,
  type ServiceSpotMessageFollowSeal,
  type ServiceUserSpotOperationHandler,
  type ServiceUserSpotOperationResult,
  type ServiceStatefulMailboxData,
  type ServiceStatefulPendingOperation,
  type ServiceStatefulResult
} from '../../foundation/service-stateful-runtime';
import type {
  ServiceActorRef,
  ServiceSpotState
} from '../../foundation/service-stateful-registry';
import type {
  ServiceActorCreateRecord,
  ServiceDirectSpotRouteFence,
  ServiceInstanceActivationTarget,
  ServiceInstanceRouteFence,
  ServiceSpotRouteFence,
  ServiceUserSpotCloseRecord,
  ServiceUserSpotCreateRecord
} from '../../foundation/service-stateful-wire-codec';
import { encodeServiceMetadataFrame } from '../../foundation/service-metadata-codec';
import type {
  ServiceInstanceActivationRecoveryEnvelope
} from '../../foundation/service-instance-activation-recovery-codec';
import type {
  ServiceMailboxClaim,
  ServiceMailboxDomain,
  ServiceMailboxRecord
} from '../../foundation/service-mailbox';
import type {
  ServiceChannelDescriptor,
  ServiceNodeDescriptor
} from '../../foundation/service-topology-registry';
import type {
  RoutingId
} from '../../../contracts';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendObjectPlacement,
  ZLinkBackendMeshNode
} from '../contracts';

const MULTIPART_PACKET_NAME = SERVICE_FRAMEWORK_MULTIPART_PACKET_NAME;
const MULTIPART_CONTENT_TYPE = SERVICE_FRAMEWORK_MULTIPART_CONTENT_TYPE;
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });
const MAX_DRAIN_RECORDS = 64;

/**
 * M6A MeshNode backend. Stateful Spot/Actor entry points stay explicit until
 * their owning M6B runtime is connected; topology and node/channel dispatch do
 * not depend on those entry points.
 */
export class ZLinkNodeRawMeshBackend implements ZLinkBackendMeshNode {
  readonly nativeInstance = this;

  private readonly channels = new Map<string, number>();
  private readonly peerIntents = new Map<bigint, {
    readonly endpoint: string;
    readonly nodeRoutingId?: string;
    readonly lifecycleGeneration?: bigint;
  }>();
  private readonly completions: Array<PendingCompletion | undefined> = [];
  private completionHead = 0;
  private completionCount = 0;
  private readonly routingId: string;
  private readonly lifecycleGeneration: bigint;
  private bindEndpoint?: string;
  private runtime?: RawServiceMeshRuntime;
  private stateful?: ServiceStatefulRuntime;
  private readyHandler?: (domains: number) => number;
  private pollTimer?: NodeJS.Timeout;
  private nextPeerIntent = 1n;
  private closed = false;
  private objectRole: ServiceNodeDescriptor['objectRole'] = 'none';
  private placementWeight = 100;
  private activeCapacityLimit = 10_000;
  private pendingCapacityLimit = 128;
  private objectCapabilities: readonly string[] = [];
  private inboundMessageDropped?: (
    surface: 'node' | 'channel',
    messageKind: 'send',
    reason: 'backpressure'
  ) => void;
  private messageFollowHandler?: (
    record: import('../../foundation/service-stateful-wire-codec').ServiceMessageFollowRecord
  ) => void;

  constructor(
    private readonly meshName: string,
    routingId: string | undefined
  ) {
    if (meshName.length === 0) throw new TypeError('MeshName must be non-empty.');
    if (routingId === undefined || routingId.length === 0) {
      throw new TypeError('M6A raw MeshNode requires a routing id.');
    }
    this.routingId = routingId;
    this.lifecycleGeneration = createLifecycleGeneration();
  }

  configureObjectPlacement(options: {
    readonly role: ServiceNodeDescriptor['objectRole'];
    readonly placementWeight: number;
    readonly activeCapacityLimit: number;
    readonly pendingCapacityLimit: number;
    readonly objectCapabilities: readonly string[];
  }): void {
    this.requireNotStarted();
    this.objectRole = options.role;
    this.placementWeight = requirePublicWeight(
      options.placementWeight,
      'placementWeight'
    );
    this.activeCapacityLimit = requirePositivePlacementValue(
      options.activeCapacityLimit,
      'activeCapacityLimit'
    );
    this.pendingCapacityLimit = requirePositivePlacementValue(
      options.pendingCapacityLimit,
      'pendingCapacityLimit'
    );
    this.objectCapabilities = [...new Set(options.objectCapabilities)].sort();
  }

  setInboundMessageDroppedHandler(handler: (
    surface: 'node' | 'channel',
    messageKind: 'send',
    reason: 'backpressure'
  ) => void): void {
    this.inboundMessageDropped = handler;
  }

  selectObjectPlacement(stableType: string): ZLinkBackendObjectPlacement {
    const runtime = this.requireRuntime();
    const localNodeRid = runtime.topology.localDescriptor().nodeRoutingId;
    const status = runtime.topology.objectPlacementStatus(
      stableType,
      candidate => candidate.nodeRoutingId === localNodeRid
        || runtime.isPeerRouteReady(candidate.nodeRoutingId)
    );
    if (status !== 'available') return { kind: status };
    const descriptor = runtime.topology.selectObjectPlacement(
      stableType,
      candidate => candidate.nodeRoutingId === localNodeRid
        || runtime.isPeerRouteReady(candidate.nodeRoutingId)
    );
    return descriptor === undefined
      ? { kind: 'unavailable' }
      : {
          kind: 'selected',
          target: {
            targetNodeRid: descriptor.nodeRoutingId,
            targetNodeGeneration: descriptor.lifecycleGeneration,
            descriptorVersion: descriptor.descriptorRevision.toString()
          }
        };
  }

  instanceSpotPlacementTypes(): readonly string[] {
    return this.requireRuntime().topology.instanceSpotPlacementTypes();
  }

  sendToMissingInstanceSpot(
    target: ServiceInstanceActivationTarget,
    parts: MessageLike | readonly MessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): SubmitResult {
    const result = this.requireStateful().sendToMissingInstanceSpotFrame(
      target,
      encodeMultipartApplicationFrame(parts),
      deadlineUnixMs,
      sourceSpotId,
      metadata === undefined ? undefined : encodeServiceMetadataFrame(metadata)
    );
    return result as SubmitResult;
  }

  prepareMissingInstanceSpotSend(
    target: ServiceInstanceActivationTarget,
    parts: MessageLike | readonly MessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): () => SubmitResult {
    const submit = this.requireStateful().prepareMissingInstanceSpotSendFrame(
      target,
      encodeMultipartApplicationFrame(parts),
      deadlineUnixMs,
      sourceSpotId,
      metadata === undefined ? undefined : encodeServiceMetadataFrame(metadata)
    );
    return () => submit() as SubmitResult;
  }

  requestToMissingInstanceSpot(
    target: ServiceInstanceActivationTarget,
    parts: MessageLike | readonly MessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.InstanceSpotRequest,
      this.requireStateful().requestToMissingInstanceSpotFrame(
        target,
        encodeMultipartApplicationFrame(parts),
        deadlineUnixMs,
        sourceSpotId,
        metadata === undefined ? undefined : encodeServiceMetadataFrame(metadata)
      )
    );
  }

  setRoutingId(routingId: unknown): void {
    if (String(routingId) !== this.routingId) {
      throw new Error('MeshNode routing id is immutable after construction.');
    }
  }

  setBind(endpoint: string): void {
    this.requireNotStarted();
    if (endpoint.length === 0) throw new TypeError('MeshNode bind endpoint must be non-empty.');
    this.bindEndpoint = endpoint;
  }

  start(): void {
    if (this.runtime !== undefined) return;
    if (this.closed) throw new Error('MeshNode is closed.');
    if (this.bindEndpoint === undefined) throw new Error('MeshNode bind endpoint is not configured.');
    const descriptor = this.createDescriptor();
    const runtime = new RawServiceMeshRuntime({
      descriptor,
      onInboundMessageDropped: (surface, messageKind, reason) =>
        this.inboundMessageDropped?.(surface, messageKind, reason),
      onPeerNotRequired: (nodeRoutingId, endpoint) => {
        for (const [intent, peer] of this.peerIntents) {
          if (
            (
              peer.nodeRoutingId === undefined
              || peer.nodeRoutingId === nodeRoutingId
            )
            && peer.endpoint === endpoint
          ) {
            this.peerIntents.delete(intent);
          }
        }
      }
    });
    runtime.start();
    this.stateful = new ServiceStatefulRuntime(
      runtime,
      descriptor.nodeRoutingId,
      descriptor.lifecycleGeneration
    );
    this.stateful.setMessageFollowHandler((record) => this.messageFollowHandler?.(record));
    this.runtime = runtime;
    this.schedulePoll();
  }

  setMessageFollowHandler(handler: (
    record: import('../../foundation/service-stateful-wire-codec').ServiceMessageFollowRecord
  ) => void): void {
    this.messageFollowHandler = handler;
  }

  sendMessageFollowNotification(
    targetNodeRid: string,
    record: Omit<
      import('../../foundation/service-stateful-wire-codec').ServiceMessageFollowRecord,
      'kind'
    >
  ): void {
    this.requireStateful().sendMessageFollowNotification(targetNodeRid, record);
  }

  shutdown(_timeoutMs: number): RequestResultValue {
    this.close();
    return RequestResult.Ok;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    if (this.pollTimer !== undefined) clearTimeout(this.pollTimer);
    this.pollTimer = undefined;
    this.stateful?.close();
    this.stateful = undefined;
    this.runtime?.close();
    this.runtime = undefined;
    this.clearCompletions();
  }

  addChannelName(name: string): void {
    this.requireNotStarted();
    if (name.length === 0) throw new TypeError('ChannelName must be non-empty.');
    this.channels.set(name, this.channels.get(name) ?? 100);
  }

  setChannelWeight(name: string, weight: number): void {
    if (!this.channels.has(name)) throw new Error(`Channel '${name}' is not registered.`);
    const validated = requirePublicWeight(weight, 'Channel weight');
    if (this.channels.get(name) === validated) return;
    this.channels.set(name, validated);
    this.runtime?.updateLocalWeights({
      channelName: name,
      channelWeight: validated
    });
  }

  setPlacementWeight(weight: number): void {
    const validated = requirePublicWeight(weight, 'Placement weight');
    if (this.placementWeight === validated) return;
    this.placementWeight = validated;
    this.runtime?.updateLocalWeights({ placementWeight: validated });
  }

  connectPeer(options: {
    readonly endpoint: string;
    readonly expectedRid?: unknown;
    readonly expectedSecurityIdentity?: string;
    readonly expectedLifecycleGeneration?: bigint;
  }): bigint {
    const runtime = this.requireRuntime();
    const nodeRoutingId = options.expectedRid === undefined
      ? undefined
      : String(options.expectedRid);
    if (nodeRoutingId === undefined) {
      runtime.connectPeerEndpoint(options.endpoint);
    } else {
      runtime.connectPeerByRoutingId(
        options.endpoint,
        nodeRoutingId,
        options.expectedSecurityIdentity
          ?? runtime.topology.localDescriptor().securityIdentity,
        options.expectedLifecycleGeneration
      );
    }
    const intent = this.nextPeerIntent++;
    this.peerIntents.set(intent, {
      endpoint: options.endpoint,
      ...(nodeRoutingId === undefined ? {} : {
        nodeRoutingId,
        lifecycleGeneration: options.expectedLifecycleGeneration
      })
    });
    if (nodeRoutingId !== undefined) runtime.announcePeer(nodeRoutingId);
    return intent;
  }

  removePeerConnection(intentId: bigint): void {
    const intent = this.peerIntents.get(intentId);
    if (intent === undefined) return;
    this.peerIntents.delete(intentId);
    if (intent.nodeRoutingId === undefined) {
      this.runtime?.disconnectPeerEndpoint(intent.endpoint);
    } else {
      this.runtime?.disconnectPeer(
        intent.endpoint,
        intent.nodeRoutingId,
        intent.lifecycleGeneration
      );
    }
  }

  disconnectPeer(peerRid: unknown, lifecycleGeneration: bigint): void {
    const nodeRoutingId = String(peerRid);
    const admitted = this.runtime?.topology.peer(nodeRoutingId);
    if (
      admitted !== undefined
      && admitted.descriptor.lifecycleGeneration !== lifecycleGeneration
    ) {
      return;
    }
    const admittedEndpoint = admitted?.descriptor.advertisedEndpoint;
    const found = [...this.peerIntents.entries()]
      .find(([, intent]) =>
        (
          intent.nodeRoutingId === nodeRoutingId
          && (
            intent.lifecycleGeneration === undefined
            || intent.lifecycleGeneration === lifecycleGeneration
          )
        )
        || (
          intent.nodeRoutingId === undefined
          && admittedEndpoint !== undefined
          && intent.endpoint === admittedEndpoint
        ));
    if (found === undefined) {
      if (admitted !== undefined) {
        this.runtime?.disconnectPeer(
          admitted.descriptor.advertisedEndpoint,
          nodeRoutingId,
          lifecycleGeneration
        );
      }
      return;
    }
    this.removePeerConnection(found[0]);
  }

  replaceDiscoveredNotRequiredPeers(peers: readonly {
    readonly nodeRoutingId: string;
    readonly lifecycleGeneration: bigint;
    readonly descriptorRevision: bigint;
    readonly endpoint: string;
  }[]): void {
    const local = this.requireRuntime().topology.localDescriptor();
    this.requireRuntime().replaceDiscoveredNotRequired(peers.map(peer => ({
      ...local,
      nodeRoutingId: peer.nodeRoutingId,
      lifecycleGeneration: peer.lifecycleGeneration,
      descriptorRevision: peer.descriptorRevision,
      advertisedEndpoint: peer.endpoint,
      channels: [],
      objectRole: 'client'
    })));
  }

  isObjectClientNodeDirectTarget(targetRid: unknown): boolean {
    return this.requireRuntime().isObjectClientNodeDirectTarget(String(targetRid));
  }

  isPeerRouteReady(targetRid: unknown, lifecycleGeneration?: bigint): boolean {
    return this.requireRuntime().isPeerRouteReady(String(targetRid), lifecycleGeneration);
  }

  sendToNode(
    targetRid: unknown,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireRuntime().sendToNode(
      String(targetRid),
      encodeMultipart(parts)
    ) ? SubmitResult.Ok : SubmitResult.NotConnected;
  }

  requestToNode(
    targetRid: unknown,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly timeoutMs?: number }
  ): MeshOperationId {
    const pending = this.requireRuntime().requestToNode(
      String(targetRid),
      encodeMultipart(parts),
      options?.timeoutMs ?? 30_000
    );
    return this.observeCompletion(pending.id, OperationKind.NodeRequest, pending.promise);
  }

  sendToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireRuntime().sendToChannel(
      channelName,
      encodeMultipart(parts)
    ) ? SubmitResult.Ok : SubmitResult.NotConnected;
  }

  requestToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly timeoutMs?: number }
  ): MeshOperationId {
    const pending = this.requireRuntime().requestToChannel(
      channelName,
      encodeMultipart(parts),
      options?.timeoutMs ?? 30_000
    );
    if (pending === undefined) {
      return this.enqueueImmediateFailure(OperationKind.ChannelRequest, RequestResult.NotFound);
    }
    return this.observeCompletion(pending.id, OperationKind.ChannelRequest, pending.promise);
  }

  status() {
    const descriptor = this.runtime?.topology.localDescriptor() ?? this.createDescriptor();
    const peers = this.runtime?.topology.peers() ?? [];
    return {
      state: stateCode(descriptor.state),
      routingId: descriptor.nodeRoutingId as RoutingId,
      meshName: descriptor.meshName,
      localEndpoint: descriptor.advertisedEndpoint,
      lifecycleGeneration: descriptor.lifecycleGeneration,
      descriptorRevision: descriptor.descriptorRevision,
      channelCount: descriptor.channels.length,
      configuredPeerCount: this.peerIntents.size,
      admittedPeerCount: peers.length,
      drainingPeerCount: peers.filter(peer => peer.descriptor.state === 'draining').length,
      pendingApplicationMessages: BigInt(this.runtime?.mailbox.pendingMessages('application') ?? 0),
      pendingInfrastructureMessages: BigInt(
        (this.runtime?.mailbox.pendingMessages('infrastructure') ?? 0) + this.completionCount
      ),
      pendingBytes: BigInt(
        (this.runtime?.mailbox.pendingBytes('application') ?? 0)
        + (this.runtime?.mailbox.pendingBytes('infrastructure') ?? 0)
      ),
      lastError: 0,
      lastChangedMs: BigInt(Math.trunc(performance.now()))
    };
  }

  peers(): MeshPeerEntry[] {
    const intents = [...this.peerIntents.entries()].map(([id, intent]) => ({
      id,
      ...intent
    }));
    const findIntent = (nodeRoutingId: string, endpoint: string) =>
      intents.find(intent =>
        intent.nodeRoutingId === nodeRoutingId
        || (
          intent.nodeRoutingId === undefined
          && intent.endpoint === endpoint
        ));
    const admitted = (this.runtime?.topology.peers() ?? []).map(peer => ({
      connectionIntentId: findIntent(
        peer.descriptor.nodeRoutingId,
        peer.descriptor.advertisedEndpoint
      )?.id ?? 0n,
      source: 1,
      state: this.runtime?.isPeerRouteReady(peer.descriptor.nodeRoutingId) === false
        ? 1
        : peerStateCode(peer.descriptor.state),
      routingId: peer.descriptor.nodeRoutingId as RoutingId,
      lifecycleGeneration: peer.descriptor.lifecycleGeneration,
      descriptorRevision: peer.descriptor.descriptorRevision,
      endpoint: peer.descriptor.advertisedEndpoint,
      channelCount: peer.descriptor.channels.length,
      lastError: 0,
      lastChangedMs: BigInt(Math.trunc(performance.now()))
    }));
    const notRequired = (this.runtime?.topology.notRequiredPeers() ?? []).map(descriptor => ({
      connectionIntentId: findIntent(
        descriptor.nodeRoutingId,
        descriptor.advertisedEndpoint
      )?.id ?? 0n,
      source: 1,
      state: 6,
      routingId: descriptor.nodeRoutingId as RoutingId,
      lifecycleGeneration: descriptor.lifecycleGeneration,
      descriptorRevision: descriptor.descriptorRevision,
      endpoint: descriptor.advertisedEndpoint,
      channelCount: 0,
      lastError: 0,
      lastChangedMs: BigInt(Math.trunc(performance.now()))
    }));
    const projected = new Set([
      ...admitted.map(peer => peer.connectionIntentId),
      ...notRequired.map(peer => peer.connectionIntentId)
    ]);
    const disconnected = intents
      .filter(intent =>
        intent.nodeRoutingId !== undefined
        && !projected.has(intent.id))
      .map(intent => {
        const descriptor = this.runtime?.topology.knownDescriptor(intent.nodeRoutingId!);
        return {
          connectionIntentId: intent.id,
          source: 1,
          // A new manual intent is Connecting until its first descriptor is
          // observed. A previously admitted required peer is NotConnected.
          state: descriptor === undefined ? 1 : 0,
          routingId: intent.nodeRoutingId! as RoutingId,
          lifecycleGeneration: descriptor?.lifecycleGeneration ?? 0n,
          descriptorRevision: descriptor?.descriptorRevision ?? 0n,
          endpoint: descriptor?.advertisedEndpoint ?? intent.endpoint,
          channelCount: descriptor?.channels.length ?? 0,
          lastError: 0,
          lastChangedMs: BigInt(Math.trunc(performance.now()))
        };
      });
    return [...admitted, ...notRequired, ...disconnected]
      .sort((left, right) => String(left.routingId).localeCompare(String(right.routingId)));
  }

  peerChannels(peerRid: unknown, lifecycleGeneration: bigint) {
    const descriptor = this.runtime?.topology.knownDescriptor(String(peerRid));
    if (descriptor === undefined || descriptor.lifecycleGeneration !== lifecycleGeneration) {
      return { names: [], weights: [] };
    }
    return {
      names: descriptor.channels.map(channel => channel.name),
      weights: descriptor.channels.map(channel => channel.weight)
    };
  }

  createPublisher(): MeshPublisher {
    let publisherClosed = false;
    const publish = (
      channelName: string,
      topic: string,
      parts: MessageLike | readonly MessageLike[]
    ): void => {
      if (publisherClosed) throw new Error('Mesh publisher is closed.');
      this.requireStateful().publishLogicalMulticast(
        channelName,
        topic,
        encodeMultipart(parts)
      );
    };
    return {
      publish,
      publishAsync: async (channelName, topic, parts, _options, signal) => {
        signal?.throwIfAborted();
        publish(channelName, topic, parts);
      },
      close: () => {
        publisherClosed = true;
      }
    };
  }

  setReadyHandler(handler: (readyDomains: number) => number): void {
    this.readyHandler = handler;
    this.notifyReady();
  }

  createReadyBatch(capacity: number): ReadyBatch {
    return new RawReadyBatch(capacity);
  }

  createReceiveBatch(
    messageCapacity: number,
    partCapacity: number,
    byteCapacity: number
  ): ReceiveBatch {
    return new RawReceiveBatch(messageCapacity, partCapacity, byteCapacity);
  }

  drainReady(
    domains: number,
    batch: ReadyBatch
  ): { readonly ok: boolean; readonly hasResidue: boolean; readonly records: readonly ReadyRecord[] } {
    const target = requireRawReadyBatch(batch);
    if ((domains & ReadyDomain.Infrastructure) !== 0) {
      this.drainCompletions(target);
      this.drainMailbox('infrastructure', target);
    }
    if ((domains & ReadyDomain.Application) !== 0) {
      this.drainMailbox('application', target);
    }
    const runtime = this.runtime;
    const infrastructureResidue = (domains & ReadyDomain.Infrastructure) !== 0
      && (
        this.completionCount > 0
        || (runtime?.mailbox.pendingMessages('infrastructure') ?? 0) > 0
      );
    const applicationResidue = (domains & ReadyDomain.Application) !== 0
      && (runtime?.mailbox.pendingMessages('application') ?? 0) > 0;
    return {
      ok: true,
      // Residue is scoped to the requested lane. The pump drains lanes in
      // separate turns and must not wait on another lane before returning.
      hasResidue: infrastructureResidue || applicationResidue,
      records: target.records
    };
  }

  createSpot(): ServiceSpot {
    return new RawServiceSpot(this, this.requireStateful().createSpot());
  }

  entrySpot(): ServiceSpot {
    return new RawServiceSpot(this, this.requireStateful().entrySpot());
  }

  getOrCreateSpot(routingId: unknown): { readonly spot: ServiceSpot; readonly created: boolean } {
    const stateful = this.requireStateful();
    const rid = String(routingId);
    const existing = stateful.registry.spot(rid);
    const state = existing ?? stateful.createSpot(rid);
    return { spot: new RawServiceSpot(this, state), created: existing === undefined };
  }

  instanceSpotApplicationTarget(
    spotId: string
  ): { readonly stableType: string; readonly objectGeneration: bigint } | undefined {
    return this.requireStateful().instanceSpotApplicationTarget(spotId);
  }

  waitForInstanceApplicationQuiescence(
    spotId: string,
    signal?: AbortSignal
  ): Promise<void> {
    return this.requireStateful().waitForInstanceApplicationQuiescence(spotId, signal);
  }

  restoreUserSpotAuthority(
    spotId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpot {
    return new RawServiceSpot(
      this,
      this.requireStateful().restoreUserSpotAuthority(
        spotId,
        stableType,
        generation,
        authorityOwnerGeneration
      )
    );
  }

  restoreSpotAuthority(
    spotId: string,
    objectKind: 'user_spot' | 'instance_spot',
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpot {
    return new RawServiceSpot(
      this,
      this.requireStateful().restoreSpotAuthority(
        spotId,
        objectKind,
        stableType,
        generation,
        authorityOwnerGeneration
      )
    );
  }

  createActor(actorId: string): ZLinkBackendActorRef {
    return this.requireStateful().createActor(actorId).ref;
  }

  restoreActorAuthority(
    actorId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint,
    spotId: string,
    spotGeneration: bigint,
    membershipEpoch: bigint
  ): ZLinkBackendActorRef {
    return this.requireStateful().restoreActorAuthority(
      actorId,
      stableType,
      generation,
      authorityOwnerGeneration,
      spotId,
      spotGeneration,
      membershipEpoch
    ).ref;
  }

  discardRelocatedActor(actor: ZLinkBackendActorRef): void {
    this.requireStateful().discardRelocatedActor({
      nodeRid: String(actor.nodeRid),
      actorId: actor.actorId,
      generation: actor.generation
    });
  }

  restoreActorSessionBinding(
    actor: ZLinkBackendActorRef,
    sessionNodeRid: unknown,
    sessionRid: unknown,
    bindingGeneration: bigint
  ): void {
    this.requireStateful().restoreActorSessionBinding(
      {
        nodeRid: String(actor.nodeRid),
        actorId: actor.actorId,
        generation: actor.generation
      },
      String(sessionNodeRid),
      String(sessionRid),
      bindingGeneration
    );
  }

  registerInstanceIntent(
    instanceType: string,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): void {
    this.requireStateful().registerInstanceIntent(instanceType, route, expectedCurrentRoute);
  }

  registerAsyncInstanceActivationAuthority(
    authority: ServiceAsyncInstanceActivationAuthority
  ): void {
    this.requireStateful().registerAsyncInstanceActivationAuthority(authority);
  }

  registerInstanceApplicationLifecycle(
    lifecycle: ServiceInstanceApplicationLifecycle
  ): void {
    this.requireStateful().registerInstanceApplicationLifecycle(lifecycle);
  }

  registerUserSpotOperationHandler(handler: ServiceUserSpotOperationHandler): void {
    this.requireStateful().registerUserSpotOperationHandler(handler);
  }

  requestUserSpotCreate(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    return this.requireStateful().requestUserSpotCreate(targetNodeRid, request, timeoutMs);
  }

  requestUserSpotClose(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCloseRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    return this.requireStateful().requestUserSpotClose(targetNodeRid, request, timeoutMs);
  }

  requestActorCreate(
    targetNodeRid: string,
    request: Omit<ServiceActorCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult> {
    return this.requireStateful().requestActorCreate(targetNodeRid, request, timeoutMs);
  }

  recoverInstanceActivation(
    envelope: ServiceInstanceActivationRecoveryEnvelope,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<boolean> {
    return this.requireStateful().recoverInstanceActivation(envelope, route, expectedCurrentRoute);
  }

  recoverPendingInstanceActivation(
    envelope: ServiceInstanceActivationRecoveryEnvelope,
    pending: ServicePendingInstanceActivation,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<boolean> {
    return this.requireStateful().recoverPendingInstanceActivation(envelope, pending, expectedCurrentRoute);
  }

  completeRecoveredInstanceActivation(
    target: ServiceInstanceActivationTarget,
    route: ServiceInstanceRouteFence,
    expectedCurrentRoute?: ServiceInstanceRouteFence | null
  ): Promise<ServiceInstanceRouteFence | undefined> {
    return this.requireStateful().completeRecoveredInstanceActivation(target, route, expectedCurrentRoute);
  }

  forgetInstanceIntent(
    spotId: string,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    storeVersion?: string
  ): void {
    this.requireStateful().forgetInstanceIntent(
      spotId,
      objectGeneration,
      authorityOwnerGeneration,
      storeVersion
    );
  }

  rememberSpotRoute(
    route: ServiceDirectSpotRouteFence,
    expectedCurrentRoute?: ServiceDirectSpotRouteFence | null
  ): void {
    this.requireStateful().rememberSpotRoute(route, expectedCurrentRoute);
  }

  entrySpotRouteFence(
    targetNodeRid: string,
    targetSpotId: string
  ): ServiceDirectSpotRouteFence {
    const generation = this.peerGeneration(targetNodeRid);
    return {
      spot: { spotId: targetSpotId, generation },
      targetNodeRid,
      targetNodeGeneration: generation,
      authorityOwnerGeneration: generation,
      ownerLeaseGeneration: generation,
      storeVersion: entrySpotFenceVersion(generation)
    };
  }

  sealSpotMessageFollowIngress(
    source: ServiceDirectSpotRouteFence
  ): ServiceSpotMessageFollowSeal | undefined {
    return this.requireStateful().sealSpotMessageFollowIngress(source);
  }

  abortSpotMessageFollowIngress(seal: ServiceSpotMessageFollowSeal): boolean {
    return this.requireStateful().abortSpotMessageFollowIngress(seal);
  }

  commitSpotMessageFollowIngress(
    seal: ServiceSpotMessageFollowSeal,
    target: ServiceDirectSpotRouteFence,
    durationMs: number
  ): Promise<boolean> {
    return this.requireStateful().commitSpotMessageFollowIngress(
      seal,
      target,
      durationMs
    );
  }

  forgetSpotRoute(
    spot: ServiceSpotRouteFence['spot'],
    authorityOwnerGeneration: bigint,
    storeVersion?: string
  ): void {
    this.requireStateful().forgetSpotRoute(spot, authorityOwnerGeneration, storeVersion);
  }

  sendToInstanceSpot(
    route: ServiceInstanceRouteFence,
    parts: MessageLike | readonly MessageLike[],
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): SubmitResult {
    const result = this.requireStateful().sendToInstanceSpot(
      route,
      encodeMultipart(parts),
      sourceSpotId,
      metadata === undefined ? undefined : encodeServiceMetadataFrame(metadata)
    );
    return result as SubmitResult;
  }

  requestInstanceSpot(
    route: ServiceInstanceRouteFence,
    parts: MessageLike | readonly MessageLike[],
    timeoutMs = 30_000,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.InstanceSpotRequest,
      this.requireStateful().requestToInstanceSpot(
        route,
        encodeMultipart(parts),
        timeoutMs,
        sourceSpotId,
        metadata === undefined ? undefined : encodeServiceMetadataFrame(metadata)
      )
    );
  }

  actorLookup(actorId: string) {
    const actor = this.requireStateful().actor(actorId);
    if (actor === undefined) {
      throw Object.assign(new Error(`Actor '${actorId}' was not found.`), { nativeErrno: 2 });
    }
    return {
      actor: actor.ref,
      spotId: actor.spot.spotId,
      spotGeneration: actor.spot.generation,
      membershipEpoch: actor.membershipEpoch
    };
  }

  lookupRemoteActor(targetNodeRid: unknown, actorId: string, timeoutMs = 30_000): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorLookup,
      this.requireStateful().lookupRemoteActor(String(targetNodeRid), actorId, timeoutMs)
    );
  }

  destroyActor(actor: ZLinkBackendActorRef, timeoutMs = 30_000): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorDestroy,
      this.requireStateful().destroyActor(actor, timeoutMs)
    );
  }

  joinActorSpot(
    actor: ZLinkBackendActorRef,
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts?: MessageLike | readonly MessageLike[],
    timeoutMs = 30_000
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorJoin,
      this.requireStateful().joinActor(
        actor,
        String(targetNodeRid),
        { spotId: String(targetSpotId), generation: targetSpotGeneration },
        targetSpotGeneration,
        parts === undefined ? undefined : encodeMultipart(parts),
        timeoutMs
      )
    );
  }

  joinActorEntrySpot(
    actor: ZLinkBackendActorRef,
    targetNodeRid: unknown,
    parts?: MessageLike | readonly MessageLike[],
    timeoutMs = 30_000
  ): MeshOperationId {
    const target = String(targetNodeRid);
    return this.observeStateful(
      OperationKind.ActorJoin,
      this.requireStateful().joinActorEntrySpot(
        actor,
        target,
        parts === undefined ? undefined : encodeMultipart(parts),
        timeoutMs
      )
    );
  }

  sendToActor(
    actor: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireStateful().sendToActor(
      actor,
      this.peerGeneration(String(actor.nodeRid)),
      actor.generation,
      encodeMultipart(parts)
    ) as SubmitResultValue;
  }

  requestToActor(
    actor: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly timeoutMs?: number }
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorRequest,
      this.requireStateful().requestToActor(
        actor,
        this.peerGeneration(String(actor.nodeRid)),
        actor.generation,
        encodeMultipart(parts),
        options?.timeoutMs ?? 30_000
      )
    );
  }

  actorSendToActor(
    source: ZLinkBackendActorRef,
    target: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireStateful().sendToActor(
      target,
      this.peerGeneration(String(target.nodeRid)),
      target.generation,
      encodeMultipart(parts),
      source
    ) as SubmitResultValue;
  }

  actorRequestToActor(
    source: ZLinkBackendActorRef,
    target: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly timeoutMs?: number }
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorRequest,
      this.requireStateful().requestToActor(
        target,
        this.peerGeneration(String(target.nodeRid)),
        target.generation,
        encodeMultipart(parts),
        options?.timeoutMs ?? 30_000,
        source
      )
    );
  }

  sendActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireStateful().sendBoundSession(
      actor,
      expectedBindingGeneration,
      encodeMultipart(parts)
    ) as SubmitResultValue;
  }

  closeActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs = 30_000
  ): MeshOperationId {
    const binding = this.requireStateful().registry.binding(actor);
    if (binding === undefined) {
      return this.enqueueImmediateFailure(OperationKind.StreamUnbind, RequestResult.NotFound);
    }
    return this.observeStateful(
      OperationKind.StreamUnbind,
      this.requireStateful().unbindSession(
        binding.sessionRid,
        actor,
        expectedBindingGeneration,
        timeoutMs
      )
    );
  }

  leaveActor(
    actor: ZLinkBackendActorRef,
    expectedMembershipEpoch: bigint,
    timeoutMs = 30_000
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.ActorLeave,
      this.requireStateful().leaveActor(actor, expectedMembershipEpoch, timeoutMs)
    );
  }

  createStreamSessionService(stream: unknown): StreamSessionService {
    return new RawStreamSessionService(
      this.requireStateful(),
      stream as StreamSocket,
      (kind, pending) => this.observeStateful(kind, pending)
    );
  }

  sendFromSpot(
    source: ServiceSpotState,
    target: ServiceDirectSpotRouteFence,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    return this.requireStateful().sendToSpot(
      source.ref.spotId,
      target,
      encodeMultipart(parts)
    ) as SubmitResultValue;
  }

  requestFromSpot(
    source: ServiceSpotState,
    target: ServiceDirectSpotRouteFence,
    parts: MessageLike | readonly MessageLike[],
    timeoutMs: number
  ): MeshOperationId {
    return this.observeStateful(
      OperationKind.SpotRequest,
      this.requireStateful().requestToSpot(
        source.ref.spotId,
        target,
        encodeMultipart(parts),
        timeoutMs
      )
    );
  }

  closeSpot(state: ServiceSpotState): boolean {
    return this.requireStateful().registry.closeSpot(state.ref);
  }

  publishFromSpot(
    state: ServiceSpotState,
    channelName: string,
    topic: string,
    parts: MessageLike | readonly MessageLike[]
  ): void {
    this.requireStateful().publishLogicalMulticast(
      channelName,
      topic,
      encodeMultipart(parts),
      state.ref.spotId
    );
  }

  setSpotSubscription(
    state: ServiceSpotState,
    channelName: string,
    topicFilter: string
  ): void {
    this.requireStateful().setSubscription(state, channelName, topicFilter);
  }

  unsetSpotSubscription(
    state: ServiceSpotState,
    channelName: string,
    topicFilter: string
  ): void {
    this.requireStateful().unsetSubscription(state, channelName, topicFilter);
  }

  clearSpotSubscriptions(state: ServiceSpotState): void {
    this.requireStateful().clearSubscriptions(state.ref.spotId);
  }

  private createDescriptor(): ServiceNodeDescriptor {
    const channels: ServiceChannelDescriptor[] = [...this.channels]
      .map(([name, weight]) => ({ name, weight }))
      .sort((left, right) => left.name.localeCompare(right.name));
    return {
      meshName: this.meshName,
      nodeRoutingId: this.routingId,
      lifecycleGeneration: this.lifecycleGeneration,
      descriptorRevision: 1n,
      advertisedEndpoint: this.bindEndpoint ?? 'inproc://not-started',
      channels,
      state: 'preparing',
      // Plaintext RouteMesh peers use the shared default admission identity.
      securityIdentity: 'default',
      effectiveMaxMessageBytes: 4 * 1024 * 1024,
      applicationVersion: 0n,
      protocolCapabilities: [
        'framework-service-v11',
        ...this.objectCapabilities
      ],
      objectRole: this.objectRole,
      placementWeight: this.placementWeight,
      activeCapacityLimit: this.activeCapacityLimit,
      pendingCapacityLimit: this.pendingCapacityLimit,
      activeCapacityUsed: 0,
      pendingCapacityUsed: 0
    };
  }

  private schedulePoll(): void {
    if (this.closed || this.pollTimer !== undefined) return;
    this.pollTimer = setTimeout(() => {
      this.pollTimer = undefined;
      try {
        this.poll();
      } finally {
        this.schedulePoll();
      }
    }, 1);
    this.pollTimer.unref();
  }

  private poll(): void {
    const runtime = this.runtime;
    if (runtime === undefined) return;
    runtime.drainMonitorEvents();
    // Completion progress is independent from Application receive. A future
    // Application HWM pause may skip pumpOne(), but must keep this call.
    runtime.progressCompletion();
    for (let index = 0; index < MAX_DRAIN_RECORDS; index++) {
      const result = runtime.pumpOne();
      if (result === 'noData') break;
      if (result === 'application') this.readyHandler?.(ReadyDomain.Application);
    }
    runtime.announceExpectedPeers();
    runtime.tickLiveness();
    this.notifyReady();
  }

  private notifyReady(): void {
    const runtime = this.runtime;
    let domains = ReadyDomain.None;
    if (this.completionCount > 0 || (runtime?.mailbox.pendingMessages('infrastructure') ?? 0) > 0) {
      domains |= ReadyDomain.Infrastructure;
    }
    if ((runtime?.mailbox.pendingMessages('application') ?? 0) > 0) {
      domains |= ReadyDomain.Application;
    }
    if (domains !== ReadyDomain.None) this.readyHandler?.(domains);
  }

  private observeCompletion(
    low: bigint,
    operationKind: number,
    promise: Promise<RawServiceRequestResult>
  ): MeshOperationId {
    const id = { high: 1n, low };
    void promise.then(
      result => this.enqueueCompletion(id, operationKind, result),
      () => this.enqueueCompletion(id, operationKind, {
        terminalResult: RequestResult.NotConnected,
        failureCode: 0
      })
    );
    return id;
  }

  private observeStateful(
    operationKind: number,
    pending: ServiceStatefulPendingOperation
  ): MeshOperationId {
    const id = { high: 2n, low: pending.id };
    void pending.promise.then(
      result => this.enqueueCompletion(id, operationKind, result),
      error => this.enqueueCompletion(id, operationKind, statefulOperationFailure(error))
    );
    return id;
  }

  private enqueueImmediateFailure(operationKind: number, terminalResult: number): MeshOperationId {
    const low = this.nextPeerIntent++;
    const id = { high: 1n, low };
    this.enqueueCompletion(id, operationKind, { terminalResult, failureCode: 0 });
    return id;
  }

  private enqueueCompletion(
    operationId: MeshOperationId,
    operationKind: number,
    result: RawServiceRequestResult | ServiceStatefulResult
  ): void {
    if (this.closed) return;
    this.completions.push({ operationId, operationKind, result });
    this.completionCount += 1;
    this.readyHandler?.(ReadyDomain.Infrastructure);
  }

  private drainCompletions(batch: RawReadyBatch): void {
    while (!batch.full && this.completionCount > 0) {
      const completion = this.takeCompletion();
      if (completion === undefined) continue;
      batch.push(
        {
          ownerKind: ReadyOwnerKind.Node,
          domain: ReadyDomain.Infrastructure,
          spotId: null,
          actor: null
        },
        new CompletionClaim(completion)
      );
    }
  }

  private takeCompletion(): PendingCompletion | undefined {
    const completion = this.completions[this.completionHead];
    if (completion === undefined) return undefined;
    this.completions[this.completionHead] = undefined;
    this.completionHead += 1;
    this.completionCount -= 1;
    if (this.completionCount === 0) {
      this.clearCompletions();
    } else if (this.completionHead >= 1024
      && this.completionHead * 2 >= this.completions.length) {
      this.completions.splice(0, this.completionHead);
      this.completionHead = 0;
    }
    return completion;
  }

  private clearCompletions(): void {
    this.completions.length = 0;
    this.completionHead = 0;
    this.completionCount = 0;
  }

  private drainMailbox(domain: ServiceMailboxDomain, batch: RawReadyBatch): void {
    const runtime = this.runtime;
    if (runtime === undefined) return;
    while (!batch.full) {
      const claim = runtime.mailbox.tryClaim(
        domain,
        MAX_DRAIN_RECORDS,
        Number.MAX_SAFE_INTEGER
      );
      if (claim === undefined) break;
      const owner = readyOwner(claim.owner, this.routingId, this.stateful, domain);
      batch.push(
        owner,
        new MailboxClaim(runtime, claim, () => {
          // A receive batch may intentionally consume fewer records than the
          // mailbox claim. Releasing the claim re-queues the remainder, so
          // expose that newly ready work to the dispatch pump.
          if (runtime.mailbox.pendingMessages(domain) > 0) {
            this.readyHandler?.(
              domain === 'infrastructure' ? ReadyDomain.Infrastructure : ReadyDomain.Application
            );
          }
        })
      );
    }
  }

  private requireRuntime(): RawServiceMeshRuntime {
    if (this.runtime === undefined) throw new Error('MeshNode is not started.');
    return this.runtime;
  }

  private requireStateful(): ServiceStatefulRuntime {
    if (this.stateful === undefined) throw new Error('Stateful MeshNode is not started.');
    return this.stateful;
  }

  private peerGeneration(nodeRid: string): bigint {
    if (nodeRid === this.routingId) {
      return this.requireRuntime().topology.localDescriptor().lifecycleGeneration;
    }
    return this.requireRuntime().topology.peer(nodeRid)?.descriptor.lifecycleGeneration ?? 1n;
  }

  private requireNotStarted(): void {
    if (this.runtime !== undefined) throw new Error('MeshNode is already started.');
    if (this.closed) throw new Error('MeshNode is closed.');
  }
}

interface PendingCompletion {
  readonly operationId: MeshOperationId;
  readonly operationKind: number;
  readonly result: RawServiceRequestResult | ServiceStatefulResult;
}

interface RawClaim {
  recvBatch(batch: ReceiveBatch): {
    readonly ok: boolean;
    readonly records: ReceiveRecord[];
  };
  release(): void;
}

class RawServiceSpot implements ServiceSpot {
  readonly routingId: RoutingId;
  readonly lifecycleGeneration: bigint;
  private readonly subscriptions = new Set<string>();
  private closed = false;

  constructor(
    private readonly backend: ZLinkNodeRawMeshBackend,
    private readonly state: ServiceSpotState
  ) {
    this.routingId = state.ref.spotId;
    this.lifecycleGeneration = state.ref.generation;
  }

  status() {
    return {
      routingId: this.routingId,
      lifecycleGeneration: this.lifecycleGeneration
    };
  }

  sendToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly flags?: number }
  ): SubmitResultValue {
    this.requireOpen();
    void options;
    return this.backend.sendToChannel(channelName, parts);
  }

  requestToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly flags?: number; readonly timeoutMs?: number }
  ): MeshOperationId {
    this.requireOpen();
    return this.backend.requestToChannel(channelName, parts, options);
  }

  sendToSpot(
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts: MessageLike | readonly MessageLike[],
    options?: {
      readonly routeFence?: ServiceDirectSpotRouteFence;
      readonly entrySpot?: boolean;
    }
  ): SubmitResultValue {
    this.requireOpen();
    const routeFence = requireDirectSpotRouteFence(
      options?.routeFence,
      String(targetNodeRid),
      String(targetSpotId),
      targetSpotGeneration,
      options?.entrySpot === true
        ? this.backend.entrySpotRouteFence(
            String(targetNodeRid),
            String(targetSpotId)
          )
        : undefined
    );
    return this.backend.sendFromSpot(
      this.state,
      routeFence,
      parts
    );
  }

  requestToSpot(
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts: MessageLike | readonly MessageLike[],
    options?: {
      readonly timeoutMs?: number;
      readonly routeFence?: ServiceDirectSpotRouteFence;
      readonly entrySpot?: boolean;
    }
  ): MeshOperationId {
    this.requireOpen();
    const routeFence = requireDirectSpotRouteFence(
      options?.routeFence,
      String(targetNodeRid),
      String(targetSpotId),
      targetSpotGeneration,
      options?.entrySpot === true
        ? this.backend.entrySpotRouteFence(
            String(targetNodeRid),
            String(targetSpotId)
          )
        : undefined
    );
    return this.backend.requestFromSpot(
      this.state,
      routeFence,
      parts,
      options?.timeoutMs ?? 30_000
    );
  }

  publish(
    channelName: string,
    topic: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { readonly flags?: number }
  ): void {
    this.requireOpen();
    void options;
    this.backend.publishFromSpot(this.state, channelName, topic, parts);
  }

  setSubscription(channelName: string, topicFilter: string, kind = 0): void {
    this.requireOpen();
    if (kind !== 0) throw new RangeError('Only the default logical multicast subscription kind is supported.');
    this.backend.setSpotSubscription(this.state, channelName, topicFilter);
    this.subscriptions.add(`${channelName}\0${topicFilter}\0${kind}`);
  }

  unsetSubscription(channelName: string, topicFilter: string, kind = 0): void {
    this.requireOpen();
    if (kind !== 0) throw new RangeError('Only the default logical multicast subscription kind is supported.');
    this.backend.unsetSpotSubscription(this.state, channelName, topicFilter);
    this.subscriptions.delete(`${channelName}\0${topicFilter}\0${kind}`);
  }

  close(): void {
    if (this.closed) return;
    if (this.state.kind !== 'entry' && !this.backend.closeSpot(this.state)) {
      throw new Error(`Spot '${this.routingId}' still has Actor members.`);
    }
    this.backend.clearSpotSubscriptions(this.state);
    this.closed = true;
    this.subscriptions.clear();
  }

  async dispose(): Promise<void> {
    this.close();
  }

  private requireOpen(): void {
    if (this.closed) throw new Error(`Spot '${this.routingId}' is closed.`);
  }
}

function requireDirectSpotRouteFence(
  route: ServiceDirectSpotRouteFence | undefined,
  targetNodeRid: string,
  targetSpotId: string,
  targetSpotGeneration: bigint,
  entryRoute: ServiceDirectSpotRouteFence | undefined
): ServiceDirectSpotRouteFence {
  if (route === undefined && entryRoute !== undefined) {
    return entryRoute;
  }
  if (
    route === undefined
    || route.targetNodeRid !== targetNodeRid
    || route.spot.spotId !== targetSpotId
    || route.spot.generation !== targetSpotGeneration
  ) {
    throw new TypeError('Direct Spot submission requires the exact resolved authority fence.');
  }
  return route;
}

function entrySpotFenceVersion(generation: bigint): string {
  return `entry:${generation}`;
}

class RawStreamSessionService implements StreamSessionService {
  private state = 1;
  private closed = false;
  private readonly sessionTargets = new Map<string, RoutingId>();

  constructor(
    private readonly stateful: ServiceStatefulRuntime,
    private readonly stream: StreamSocket,
    private readonly observe: (
      operationKind: number,
      pending: ServiceStatefulPendingOperation
    ) => MeshOperationId
  ) {}

  start(): void {
    if (this.closed) throw new Error('STREAM session service is closed.');
    this.state = 2;
  }

  shutdown(_timeoutMs: number): RequestResultValue {
    this.close();
    return RequestResult.Ok;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.state = 5;
    this.sessionTargets.clear();
  }

  status() {
    const bindings = this.allBindings();
    return {
      state: this.state,
      lifecycleGeneration: 1n,
      sessionCount: BigInt(new Set(bindings.map(value => value.sessionRid)).size),
      bindingCount: BigInt(bindings.length),
      pendingMessageCount: 0n,
      pendingByteCount: 0n,
      lastError: 0
    };
  }

  lookupActor(
    targetNodeRid: RoutingId,
    actorId: string,
    timeoutMs = 30_000
  ): MeshOperationId {
    this.requireStarted();
    return this.observe(
      OperationKind.ActorLookup,
      this.stateful.lookupRemoteActor(String(targetNodeRid), actorId, timeoutMs)
    );
  }

  bindActor(
    sessionRid: RoutingId,
    actor: ServiceActorRef,
    timeoutMs = 30_000
  ): MeshOperationId {
    this.requireStarted();
    const sessionKey = String(sessionRid);
    this.sessionTargets.set(sessionKey, sessionRid);
    return this.observe(
      OperationKind.StreamBind,
      this.stateful.bindSession(
        sessionKey,
        actor,
        timeoutMs,
        (targetSessionRid, payloadFrame) => this.deliver(targetSessionRid, payloadFrame)
      )
    );
  }

  unbindActor(
    sessionRid: RoutingId,
    actor: ServiceActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs = 30_000
  ): MeshOperationId {
    this.requireStarted();
    return this.observe(
      OperationKind.StreamUnbind,
      this.stateful.unbindSession(
        String(sessionRid),
        actor,
        expectedBindingGeneration,
        timeoutMs
      )
    );
  }

  bindings(sessionRid: RoutingId) {
    return this.stateful.sessionBindings(String(sessionRid));
  }

  sendToActor(
    sessionRid: RoutingId,
    actor: ServiceActorRef,
    parts: MessageLike | readonly MessageLike[]
  ): SubmitResultValue {
    this.requireStarted();
    return this.stateful.sendSessionToActor(
      String(sessionRid),
      actor,
      encodeMultipart(parts)
    ) as SubmitResultValue;
  }

  private deliver(sessionRid: string, payload: Uint8Array): boolean {
    const target = this.sessionTargets.get(sessionRid);
    if (target === undefined) return false;
    try {
      const operation = this.stream.send(target as unknown as BindingRoutingId);
      const parts = decodeMultipartBuffers(decodeApplicationPayloadView(payload).payload);
      if (parts.length === 0) return false;
      let submit = operation.message(parts[0]!);
      for (const part of parts.slice(1)) submit = submit.message(part);
      const delivered = submit.submit();
      if (!delivered) this.sessionTargets.delete(sessionRid);
      return delivered;
    } catch {
      // A client may close its STREAM between the binding lookup and this
      // one-way delivery. Treat that transport transition as a closed
      // session; it must not escape the runtime poll and terminate the host.
      this.sessionTargets.delete(sessionRid);
      return false;
    }
  }

  private allBindings() {
    return this.stateful.allSessionBindings();
  }

  private requireStarted(): void {
    if (this.state !== 2 || this.closed) {
      throw new Error('STREAM session service is not started.');
    }
  }
}

class RawReadyBatch implements ReadyBatch {
  readonly records: ReadyRecord[] = [];
  private readonly claims: RawClaim[] = [];

  constructor(private readonly capacity: number) {
    if (!Number.isInteger(capacity) || capacity < 1) throw new RangeError('Ready capacity must be positive.');
  }

  get full(): boolean {
    return this.records.length >= this.capacity;
  }

  push(record: ReadyRecord, claim: RawClaim): void {
    if (this.full) throw new Error('Ready batch is full.');
    this.records.push(record);
    this.claims.push(claim);
  }

  takeClaim(index: number) {
    if (index < 0 || index >= this.claims.length) {
      throw new RangeError('Ready claim index is invalid.');
    }
    return this.claims[index]!;
  }

  reset(): void {
    this.records.length = 0;
    this.claims.length = 0;
  }

  close(): void {
    this.reset();
  }
}

class RawReceiveBatch implements ReceiveBatch {
  constructor(
    readonly messageCapacity: number,
    readonly partCapacity: number,
    readonly byteCapacity: number
  ) {
    if ([messageCapacity, partCapacity, byteCapacity].some(value => !Number.isInteger(value) || value < 1)) {
      throw new RangeError('Receive batch capacities must be positive.');
    }
  }

  reset(): void {}
  close(): void {}
}

class MailboxClaim implements RawClaim {
  private consumed = false;
  private released = false;
  private remaining: ServiceMailboxRecord[] = [];

  constructor(
    private readonly runtime: RawServiceMeshRuntime,
    private readonly claim: ServiceMailboxClaim,
    private readonly onRelease?: () => void
  ) {}

  recvBatch(batch: ReceiveBatch) {
    if (this.consumed) return { ok: false, records: [] };
    this.consumed = true;
    const capacity = batch as RawReceiveBatch;
    const records: ReceiveRecord[] = [];
    let parts = 0;
    let bytes = 0;
    for (let index = 0; index < this.claim.records.length; index += 1) {
      const record = this.claim.records[index]!;
      let decoded: ReceiveRecord;
      try {
        decoded = decodeMultipartRecord(this.runtime, record);
      } catch (error) {
        if (error instanceof ServiceWireProtocolError) continue;
        throw error;
      }
      const nextParts = decoded.parts.length;
      const nextBytes = decoded.parts.reduce((sum, part) => sum + part.size(), 0);
      if (
        records.length >= capacity.messageCapacity
        || parts + nextParts > capacity.partCapacity
        || bytes + nextBytes > capacity.byteCapacity
      ) {
        for (const part of decoded.parts) part.close();
        this.remaining = this.claim.records.slice(index);
        break;
      }
      parts += nextParts;
      bytes += nextBytes;
      records.push(decoded);
    }
    return { ok: records.length > 0, records };
  }

  release(): void {
    if (this.released) return;
    this.released = true;
    this.runtime.mailbox.release(this.claim, this.remaining);
    this.onRelease?.();
  }
}

class CompletionClaim implements RawClaim {
  private consumed = false;

  constructor(private readonly completion: PendingCompletion) {}

  recvBatch(): { readonly ok: boolean; readonly records: ReceiveRecord[] } {
    if (this.consumed) return { ok: false, records: [] };
    this.consumed = true;
    const payload = this.completion.result.payload;
    return {
      ok: true,
      records: [{
        kind: ReceiveKind.Completion,
        domain: ReadyDomain.Infrastructure,
        sourceNodeRid: null,
        sourceSpotId: null,
        sourceBindingGeneration: 0n,
        sourceActor: null,
        operationId: this.completion.operationId,
        operationKind: this.completion.operationKind,
        channelName: null,
        topic: null,
        applicationMetadata: null,
        kindData: 'kindData' in this.completion.result
          ? this.completion.result.kindData ?? null
          : null,
        terminalResult: this.completion.result.terminalResult,
        failureErrno: this.completion.result.failureCode,
        parts: payload === undefined ? [] : decodeMultipart(payload.payload),
        reply: () => SubmitResult.InvalidState,
        replyActorJoin: () => SubmitResult.NotSupported
      }]
    };
  }

  release(): void {}
}

function decodeMultipartRecord(
  runtime: RawServiceMeshRuntime,
  record: ServiceMailboxRecord
): ReceiveRecord {
  const stateful = statefulMailboxData(record);
  if (stateful !== undefined) {
    return decodeStatefulRecord(record, stateful);
  }
  const header = record.parts[0]!;
  const command = header[3]!;
  const payloadFrame = record.parts[1]!;
  const application = decodeApplicationEnvelope(payloadFrame);
  const channelName = record.owner.startsWith('channel:') ? record.owner.slice('channel:'.length) : null;
  const operationId = record.correlation === undefined
    ? { high: 0n, low: 0n }
    : { high: 1n, low: record.correlation };
  const kind = command === 16
    ? ReceiveKind.NodeSend
    : command === 17
      ? ReceiveKind.NodeRequest
      : command === 18
        ? ReceiveKind.ChannelSend
        : ReceiveKind.ChannelRequest;
  const operationKind = kind === ReceiveKind.NodeRequest
    ? OperationKind.NodeRequest
    : kind === ReceiveKind.ChannelRequest
      ? OperationKind.ChannelRequest
      : 0;
  return {
    kind,
    domain: readyDomain(record.domain),
    sourceNodeRid: record.sourceRoutingId as RoutingId | undefined ?? null,
    sourceSpotId: null,
    sourceBindingGeneration: 0n,
    sourceActor: null,
    operationId,
    operationKind,
    channelName,
    topic: null,
    applicationMetadata: null,
    packetName: application.packetName,
    contentType: application.contentType,
    kindData: null,
    terminalResult: 0,
    failureErrno: 0,
    parts: decodeMultipart(application.payload),
    reply(parts) {
      if (record.correlation === undefined) return SubmitResult.InvalidState;
      runtime.reply(record, encodeMultipart(parts));
      return SubmitResult.Ok;
    },
    replyActorJoin: () => SubmitResult.NotSupported
  };
}

function decodeStatefulRecord(
  record: ServiceMailboxRecord,
  stateful: ServiceStatefulMailboxData
): ReceiveRecord {
  const payloadFrame = record.parts.length < 2 ? undefined : record.parts[1];
  const application = payloadFrame === undefined
    ? undefined
    : decodeApplicationEnvelope(payloadFrame);
  const operationId = record.correlation === undefined
    ? { high: 0n, low: 0n }
    : { high: 2n, low: record.correlation };
  return {
    kind: stateful.receiveKind,
    domain: readyDomain(record.domain),
    sourceNodeRid: record.sourceRoutingId as RoutingId | undefined ?? null,
    sourceSpotId: stateful.sourceSpotId as RoutingId | undefined ?? null,
    sourceBindingGeneration: stateful.sourceBindingGeneration ?? 0n,
    sourceActor: stateful.sourceActor ?? null,
    operationId,
    operationKind: stateful.operationKind,
    channelName: stateful.channelName ?? null,
    topic: stateful.topic ?? null,
    applicationMetadata: stateful.applicationMetadata ?? null,
    ...(application === undefined
      ? {}
      : {
          packetName: application.packetName,
          contentType: application.contentType
        }),
    kindData: stateful.kindData ?? null,
    terminalResult: 0,
    failureErrno: 0,
    parts: application === undefined ? [] : decodeMultipart(application.payload),
    ...(stateful.isPending === undefined ? {} : { isPending: stateful.isPending }),
    ...(stateful.deadlineUnixMs === undefined ? {} : { deadlineUnixMs: stateful.deadlineUnixMs }),
    ...(stateful.messageFollowOrigin === undefined
      ? {}
      : { messageFollowOrigin: stateful.messageFollowOrigin }),
    ...(stateful.onTerminalCompletion === undefined
      ? {}
      : { onTerminalCompletion: stateful.onTerminalCompletion }),
    reply(parts) {
      if (stateful.reply === undefined) return SubmitResult.InvalidState;
      return stateful.reply(
        RequestResult.Ok,
        0,
        encodeMultipart(parts)
      ) ? SubmitResult.Ok : SubmitResult.InvalidState;
    },
    replyActorJoin(joinResult, parts) {
      if (stateful.reply === undefined || stateful.targetSpot === undefined) {
        return SubmitResult.InvalidState;
      }
      const membershipEpoch = stateful.kindData?.kind === 'actorControl'
        ? stateful.kindData.currentMembershipEpoch
        : undefined;
      if (joinResult === 0 && membershipEpoch === undefined) {
        return SubmitResult.InvalidState;
      }
      const replyParts = Array.isArray(parts) ? parts : [parts];
      const accepted = stateful.reply(
        RequestResult.Ok,
        0,
        replyParts.length === 0 ? undefined : encodeMultipart(replyParts),
        {
          kind: 'actorJoin',
          joinResult: joinResult === 0 ? 0 : 1,
          spot: stateful.targetSpot,
          ...(membershipEpoch === undefined ? {} : { membershipEpoch })
        }
      );
      return accepted ? SubmitResult.Ok : SubmitResult.InvalidState;
    }
  };
}

function readyOwner(
  owner: string,
  nodeRid: string,
  stateful: ServiceStatefulRuntime | undefined,
  domain: ServiceMailboxDomain
): ReadyRecord {
  if (owner.startsWith('spot:')) {
    return {
      ownerKind: ReadyOwnerKind.Spot,
      domain: readyDomain(domain),
      spotId: owner.slice('spot:'.length),
      actor: null
    };
  }
  if (owner.startsWith('actor:')) {
    const separator = owner.indexOf('\0', 'actor:'.length);
    const actorId = separator < 0
      ? owner.slice('actor:'.length)
      : owner.slice('actor:'.length, separator);
    const generation = separator < 0 ? 0n : BigInt(owner.slice(separator + 1));
    const current = stateful?.actor(actorId);
    return {
      ownerKind: ReadyOwnerKind.Actor,
      domain: readyDomain(domain),
      spotId: current?.spot.spotId ?? null,
      actor: current?.ref ?? {
        nodeRid,
        actorId,
        generation
      }
    };
  }
  return {
    ownerKind: ReadyOwnerKind.Node,
    domain: readyDomain(domain),
    spotId: null,
    actor: null
  };
}

function readyDomain(domain: ServiceMailboxDomain): number {
  return domain === 'infrastructure'
    ? ReadyDomain.Infrastructure
    : ReadyDomain.Application;
}

function statefulOperationFailure(error: unknown): RawServiceRequestResult {
  if (error instanceof OperationTimeoutError) {
    return { terminalResult: RequestResult.TimedOut, failureCode: 0 };
  }
  if (isZLinkBackendResultError(error)) {
    return {
      terminalResult: error.result,
      failureCode: error.nativeErrno ?? 0
    };
  }
  if (error instanceof OperationCancelledError) {
    return { terminalResult: RequestResult.NotConnected, failureCode: 0 };
  }
  return { terminalResult: RequestResult.InternalError, failureCode: 17 };
}

function encodeMultipart(parts: MessageLike | readonly MessageLike[]) {
  const values = Array.isArray(parts) ? parts : [parts];
  if (values.length === 0) throw new TypeError('Multipart payload must contain at least one part.');
  const bytes = values.map(messageBytes);
  const size = 4 + bytes.reduce((sum, part) => sum + 4 + part.byteLength, 0);
  const payload = Buffer.alloc(size);
  payload.writeUInt32BE(bytes.length, 0);
  let offset = 4;
  for (const part of bytes) {
    payload.writeUInt32BE(part.byteLength, offset);
    offset += 4;
    part.copy(payload, offset);
    offset += part.byteLength;
  }
  return {
    packetName: MULTIPART_PACKET_NAME,
    contentType: MULTIPART_CONTENT_TYPE,
    payload
  };
}

function encodeMultipartApplicationFrame(
  parts: MessageLike | readonly MessageLike[]
): Buffer {
  const values = Array.isArray(parts) ? parts : [parts];
  if (values.length === 0) throw new TypeError('Multipart payload must contain at least one part.');
  return encodeMultipartApplicationPayload(
    values.map(messageBytesView),
    MULTIPART_PACKET_NAME,
    MULTIPART_CONTENT_TYPE
  );
}

function decodeApplicationEnvelope(frame: Uint8Array) {
  const bytes = Buffer.from(frame.buffer, frame.byteOffset, frame.byteLength);
  if (bytes.length < 11 || bytes[0] !== 1) {
    throw new ServiceWireProtocolError('Invalid application envelope.');
  }
  const bodyLength = bytes.readUInt32BE(1);
  if (bodyLength !== bytes.length - 5) {
    throw new ServiceWireProtocolError('Invalid application envelope length.');
  }
  let offset = 5;
  const packetLength = bytes[offset++]!;
  if (packetLength === 0 || bytes.length - offset < packetLength + 1) {
    throw new ServiceWireProtocolError('Invalid application envelope packet name.');
  }
  let packetName: string;
  try {
    packetName = FATAL_UTF8.decode(bytes.subarray(offset, offset + packetLength));
  } catch {
    throw new ServiceWireProtocolError('Invalid application envelope packet name.');
  }
  offset += packetLength;
  const contentLength = bytes[offset++]!;
  if (contentLength === 0 || bytes.length - offset < contentLength + 4) {
    throw new ServiceWireProtocolError('Invalid application envelope content type.');
  }
  let contentType: string;
  try {
    contentType = FATAL_UTF8.decode(bytes.subarray(offset, offset + contentLength));
  } catch {
    throw new ServiceWireProtocolError('Invalid application envelope content type.');
  }
  offset += contentLength;
  const payloadLength = bytes.readUInt32BE(offset);
  offset += 4;
  if (
    packetName !== MULTIPART_PACKET_NAME
    || contentType !== MULTIPART_CONTENT_TYPE
    || payloadLength !== bytes.length - offset
  ) {
    throw new ServiceWireProtocolError('Unexpected M6A application payload.');
  }
  return { packetName, contentType, payload: bytes.subarray(offset) };
}

function decodeMultipart(payload: Uint8Array): Message[] {
  const buffers = decodeMultipartBuffers(payload);
  return buffers.map(part => Message.from(part));
}

function decodeMultipartBuffers(payload: Uint8Array): Buffer[] {
  const bytes = Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength);
  if (bytes.length < 4) throw new ServiceWireProtocolError('Truncated multipart payload.');
  const count = bytes.readUInt32BE(0);
  if (count === 0 || count > Math.floor((bytes.length - 4) / 4)) {
    throw new ServiceWireProtocolError('Invalid multipart part count.');
  }
  const result = new Array<Buffer>(count);
  let offset = 4;
  for (let index = 0; index < count; index++) {
    if (bytes.length - offset < 4) {
      throw new ServiceWireProtocolError('Truncated multipart part length.');
    }
    const length = bytes.readUInt32BE(offset);
    offset += 4;
    if (bytes.length - offset < length) {
      throw new ServiceWireProtocolError('Truncated multipart part.');
    }
    result[index] = bytes.subarray(offset, offset + length);
    offset += length;
  }
  if (offset !== bytes.length) {
    throw new ServiceWireProtocolError('Multipart payload has trailing bytes.');
  }
  return result;
}

function messageBytes(value: MessageLike): Buffer {
  if (typeof value === 'object' && 'data' in value) {
    return Buffer.from(value.data());
  }
  return Buffer.from(value);
}

function messageBytesView(value: MessageLike): Uint8Array {
  if (typeof value === 'object' && 'data' in value) {
    return value.data();
  }
  if (typeof value === 'string') return Buffer.from(value);
  return value;
}

function requireRawReadyBatch(batch: ReadyBatch): RawReadyBatch {
  if (!(batch instanceof RawReadyBatch)) throw new TypeError('Ready batch belongs to another backend.');
  return batch;
}

function stateCode(state: ServiceNodeDescriptor['state']): number {
  return ['preparing', 'serving', 'retiring', 'draining', 'stopped', 'error'].indexOf(state) + 1;
}

function peerStateCode(state: ServiceNodeDescriptor['state']): number {
  switch (state) {
    case 'preparing': return 2;
    case 'serving':
    case 'retiring': return 3;
    case 'draining': return 4;
    case 'stopped':
    case 'error': return 5;
  }
}

function createLifecycleGeneration(): bigint {
  // Node's public Spot context exposes this lifecycle token as a number. Keep
  // the opaque equality token within the exact range that surface can carry;
  // lifecycle ordering is never inferred from its numeric value.
  const generation = randomBytes(8).readBigUInt64BE()
    & BigInt(Number.MAX_SAFE_INTEGER);
  return generation === 0n ? 1n : generation;
}

function requirePositivePlacementValue(value: number, name: string): number {
  if (!Number.isSafeInteger(value) || value <= 0 || value > 0x7fff_ffff) {
    throw new RangeError(`${name} must be an integer in 1..2147483647.`);
  }
  return value;
}

function requirePublicWeight(value: number, name: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new RangeError(`${name} must be an integer in 0..10000.`);
  }
  return value;
}
