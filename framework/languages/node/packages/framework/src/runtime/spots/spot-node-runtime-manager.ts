import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import { randomUUID } from 'node:crypto';
import type { ZLinkLocationOptionOverrides } from '../../contracts/Locations/Options';
import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkChannelClient,
  ZLinkFanoutClient,
  ZLinkMessage,
  ZLinkMeshNodeDescriptor,
  ZLinkMessageSerializer,
  ZLinkSpotPublisherClient
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import {
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteStatus
} from '../../contracts/Locations';
import {
  ZLinkFrameworkRuntimeState,
  ZLinkFrameworkException,
  ZLinkObjectRole
} from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import {
  SubmitResult,
  isZLinkBackendResultError
} from '../backend/runtime-values';
import {
  ReceiveKind,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import {
  ZLinkConfigurationException,
  type ZLinkFrameworkRegistration,
  type ZLinkSpotNodeOptions
} from '../configuration';
import type {
  ZLinkBackendAdapterFactory,
  ZLinkBackendContext,
  ZLinkBackendMeshNode,
  ZLinkBackendSpotNode
} from '../backend/contracts';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations/Writes';
import { ZLinkMeshDispatchPump } from '../backend/mesh-dispatch-pump';
import { ZLinkMeshCompletionTable } from '../backend/mesh-completion-table';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  encodeChannelErrorReplyParts,
  encodeChannelPublishEnvelopeParts,
  encodeChannelReplyParts
} from '../channels/channel-envelope';
import {
  ZLinkAutoConnectLoop,
  ZLinkAutoConnectReconciler,
  ZLinkLocationRuntime,
  type ZLinkLocationEventSink,
  type ZLinkLocationRuntimeStores
} from '../locations';
import type { ZLinkRemoteBoundSessionTarget } from '../actors';
import type { ZLinkActorHandoffPacket } from '../actors/actor-handoff';
import type { ZLinkDetachedTaskRunner } from './spot-actor-join-dispatch';
import { ZLinkEntrySpotActivation } from './spot-entry-activation';
import {
  createSpotNodeLocationAutoConnectContext,
  spotNodeAutoConnectCapability,
  type ZLinkSpotNodeLocationAutoConnectContext
} from './spot-node-autoconnect';
import type { ZLinkSpotRoutedTransport } from './spot-outbound';
import type { ZLinkSpotRouteResolver } from './spot-routing-internal';
import { routingIdsEqual, toBackendRoutingId } from '../routing-id';
import type {
  ZLinkEntryActorRuntime,
  ZLinkSpotActorTransferRuntime,
  ZLinkSpotActorHandoffRuntime,
  ZLinkSpotBoundSessionRuntime
} from './spot-runtime-ports';
import { createAbortError } from '../abort';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';
import type { ServiceMessageFollowRecord } from '../foundation/service-stateful-wire-codec';

const ZLINK_SEND_DONT_WAIT = 1;

export interface ZLinkSpotNodeRuntimeManagerOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly primaryMeshName?: string;
  readonly backendAdapterFactory: ZLinkBackendAdapterFactory;
  readonly context: ZLinkBackendContext;
  readonly channelClient?: ZLinkChannelClient;
  readonly fanoutClient?: ZLinkFanoutClient;
  readonly spotPublisherClient?: ZLinkSpotPublisherClient;
  readonly spotRouteResolver?: ZLinkSpotRouteResolver;
  readonly routedTransport?: ZLinkSpotRoutedTransport;
  readonly spotRouterChannelIdForMesh?: (meshName: string) => string;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly entryActorRuntime?: ZLinkEntryActorRuntime;
  readonly actorTransferRuntime?: ZLinkSpotActorTransferRuntime;
  readonly boundSessionRuntime?: ZLinkSpotBoundSessionRuntime;
  readonly actorHandoffRuntime?: ZLinkSpotActorHandoffRuntime;
  readonly detachedTaskRunner?: ZLinkDetachedTaskRunner;
  readonly meshRecordDispatcher?: (
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord
  ) => void | Promise<void>;
  readonly inboundDispatchBudget?: ZLinkInboundDispatchBudget;
  readonly messageFollowReceiver?: (record: ServiceMessageFollowRecord) => void;
}

interface ZLinkPublishSlotWaiter {
  readonly resolve: (acquired: boolean) => void;
  readonly reject: (error: unknown) => void;
  readonly signal?: AbortSignal;
  abortHandler?: () => void;
  timeout?: ReturnType<typeof setTimeout>;
  deadlineMs?: number;
  settled: boolean;
}

interface ZLinkPublishSlotQueue {
  readonly waiters: Array<ZLinkPublishSlotWaiter | undefined>;
  head: number;
  count: number;
}

export class ZLinkSpotNodeRuntimeManager {
  private readonly meshNodes = new Map<string, ZLinkBackendMeshNode>();
  private readonly meshPumps = new Map<string, ZLinkMeshDispatchPump>();
  private readonly meshCompletions = new Map<string, ZLinkMeshCompletionTable>();
  private readonly entryActivations = new Map<string, ZLinkEntrySpotActivation>();
  private readonly entryActivationStarts = new Map<string, Promise<void>>();
  private readonly publishers = new Map<
    string,
    ReturnType<ZLinkBackendMeshNode['createPublisher']>
  >();
  private readonly activePublishes = new Set<string>();
  private readonly publishSlotWaiters = new Map<string, ZLinkPublishSlotQueue>();
  private disposed = false;
  private readonly autoConnectLoops: ZLinkAutoConnectLoop[] = [];
  private readonly publishedMeshNodeDescriptors =
    new Map<string, ZLinkMeshNodeDescriptor>();
  // Keep the revision source separate from the cached Store row. A new owner
  // lease may require a NewClaim against an empty Store, while peers still
  // retain the monotonic revision from this MeshNode lifecycle.
  private readonly descriptorRevisionByMesh = new Map<string, bigint>();
  private publishedOwnerToken?: ZLinkLocationOwnerToken;
  private readonly runtimePlacementWeights = new Map<string, number>();
  private readonly runtimeChannelWeights = new Map<string, Map<string, number>>();
  private readonly entrySpotIds = new Map<string, string>();
  private runtimeWeightPublication = Promise.resolve();
  private locationAutoConnect?: ZLinkSpotNodeLocationAutoConnectContext;

  constructor(private readonly options: ZLinkSpotNodeRuntimeManagerOptions) {}

  createReceived() {
    return this.options.backendAdapterFactory.createReceived();
  }

  createTopicMessage() {
    return this.options.backendAdapterFactory.createTopicMessage();
  }

  configureLocationAutoConnect(
    runtime: ZLinkLocationRuntime,
    stores: ZLinkLocationRuntimeStores,
    options: ZLinkLocationOptionOverrides,
    events?: ZLinkLocationEventSink
  ): void {
    this.locationAutoConnect = createSpotNodeLocationAutoConnectContext(runtime, stores, options, events);
  }

  async startLocationAutoConnect(signal?: AbortSignal): Promise<void> {
    const location = this.locationAutoConnect;
    if (location === undefined || this.autoConnectLoops.length > 0) {
      return;
    }
    try {
      for (const [spotNodeName, spotNode] of this.options.registration.spotNodes.entries()) {
        const node = this.meshNodes.get(spotNodeName);
        if (node === undefined) {
          continue;
        }
        const capability = spotNodeAutoConnectCapability(spotNodeName, spotNode, node);
        if (capability === undefined) continue;
        const reconciler = new ZLinkAutoConnectReconciler({
          local: capability.local,
          localRow: capability.localRow,
          runtime: location.runtime,
          peerResolver: location.resolver,
          executor: capability.executor,
          events: location.events,
          options: location.options
        });
        const loop = new ZLinkAutoConnectLoop({
          reconciler,
          local: capability.local,
          options: location.options,
          changeStampStore: location.changeStampStore,
          leaseTracker: location.leaseTracker
        });
        await loop.start(signal);
        this.autoConnectLoops.push(loop);
      }
    } catch (error) {
      await Promise.allSettled(this.autoConnectLoops.map((loop) => loop.stop(signal)));
      this.autoConnectLoops.length = 0;
      throw error;
    }
  }

  async start(): Promise<void> {
    if (this.options.registration.spotNodes.size === 0) {
      return;
    }
    const meshAdapter = this.options.backendAdapterFactory.createMeshAdapter();
    for (const [spotNodeName, spotNode] of this.options.registration.spotNodes.entries()) {
      const routingId = spotNode.routingId
        ?? spotNode.router?.routingId
        ?? spotNode.pubSub?.routingId
        ?? `${spotNode.routingIdPrefix ?? spotNodeName}-${randomUUID()}`;
      const bind = spotNode.router?.bind;
      if (bind === undefined) {
        throw new ZLinkConfigurationException(
          `MeshNode '${spotNodeName}' requires a routing id and ROUTER bind endpoint.`
        );
      }
      const node = meshAdapter.createMeshNode(this.options.context, {
        meshName: spotNodeName,
        routingId
      });
      let pump: ZLinkMeshDispatchPump | undefined;
      const completions = new ZLinkMeshCompletionTable();
      try {
        node.setBind(bind);
        const stableTypes = [
          ...Object.keys(spotNode.spotFactoryRegistrations ?? {}),
          ...Object.keys(spotNode.instanceSpotFactoryRegistrations ?? {}),
          ...Object.keys(spotNode.actorFactoryRegistrations ?? {})
        ];
        const instanceSpotTypes = Object.keys(
          spotNode.instanceSpotFactoryRegistrations ?? {}
        );
        const hasLegacyObjectFactories =
          (spotNode.spotFactories?.length ?? 0) > 0
          || Object.keys(spotNode.instanceSpotFactories ?? {}).length > 0
          || (
            spotNode.actorFactories instanceof Map
              ? spotNode.actorFactories.size
              : Object.keys(spotNode.actorFactories ?? {}).length
          ) > 0;
        node.configureObjectPlacement({
          role: spotNode.objectRole
            ?? (hasLegacyObjectFactories ? 'server' : 'none'),
          placementWeight: spotNode.placementWeight ?? 100,
          activeCapacityLimit:
            (spotNode.actorLimit ?? 10_000) + (spotNode.spotLimit ?? 128),
          pendingCapacityLimit: spotNode.activationConcurrencyLimit ?? 128,
          objectCapabilities: [
            ...stableTypes.map(type => `object-type:${type}`),
            ...instanceSpotTypes.map(type => `instance-spot-type:${type}`)
          ]
        });
        for (const [channelName, channel] of Object.entries(spotNode.meshChannels ?? {})) {
          if (channel.server !== true) {
            continue;
          }
          node.addChannelName(channelName);
          if (channel.weight !== undefined) {
            node.setChannelWeight(channelName, channel.weight);
          }
        }
        node.setMessageFollowHandler?.((record) => this.options.messageFollowReceiver?.(record));
        node.start();
        this.publishers.set(spotNodeName, node.createPublisher());
        for (const endpoint of spotNode.router?.manualConnections ?? []) {
          node.connectPeer({ endpoint });
        }
        for (const peer of spotNode.router?.manualPeerConnections ?? []) {
          node.connectPeer({
            endpoint: peer.endpoint,
            expectedRid: toBackendRoutingId(peer.peerRid)
          });
        }
        pump = new ZLinkMeshDispatchPump(node, {
          inboundDispatchBudget: this.options.inboundDispatchBudget,
          dispatch: (owner, record) => this.dispatchMeshRecord(spotNodeName, owner, record),
          reportError: (error) => this.options.dispatchErrors?.report({
            surface: ZLinkDispatchErrorSurface.RouteMeshChannel,
            messageKind: ZLinkDispatchMessageKind.Send,
            reason: ZLinkDispatchErrorReason.HandlerException,
            action: ZLinkDispatchErrorAction.Drop,
            channelName: spotNodeName,
            error
          })
        });
        pump.start();
        this.meshNodes.set(spotNodeName, node);
        this.meshPumps.set(spotNodeName, pump);
        this.meshCompletions.set(spotNodeName, completions);
      } catch (error) {
        this.entryActivations.delete(spotNodeName);
        this.publishers.get(spotNodeName)?.close();
        this.publishers.delete(spotNodeName);
        this.meshCompletions.delete(spotNodeName);
        this.meshPumps.delete(spotNodeName);
        this.meshNodes.delete(spotNodeName);
        completions.dispose(error);
        await pump?.dispose();
        node.close();
        throw error;
      }
    }
  }

  async publishMeshNodeState(
    state: ZLinkFrameworkRuntimeState,
    signal?: AbortSignal,
    selectedMeshName?: string
  ): Promise<void> {
    const location = this.locationAutoConnect;
    const owner = location?.runtime.currentOwnerToken;
    if (location === undefined || owner === undefined) return;
    const ownerChanged = this.publishedOwnerToken !== undefined
      && !sameOwnerToken(this.publishedOwnerToken, owner);
    if (ownerChanged) {
      // A new owner token can refer to an empty or replaced Store. The cached
      // descriptor was written under the previous lease and must not force a
      // Renew against a row that no longer exists.
      this.publishedMeshNodeDescriptors.clear();
    }
    for (const [meshName, registration] of this.options.registration.spotNodes) {
      if (selectedMeshName !== undefined && meshName !== selectedMeshName) continue;
      const node = this.meshNodes.get(meshName);
      if (node === undefined) continue;
      const status = node.status();
      const userSpots = effectiveUserSpotRegistrations(registration);
      const instanceSpots = effectiveInstanceSpotRegistrations(registration);
      const actors = effectiveActorRegistrations(registration);
      const capabilities = [
        ...userSpots.map(([stableType, factory]) =>
          objectCapability('user_spot', stableType, factory)),
        ...instanceSpots.map(([stableType, factory]) =>
          objectCapability('instance_spot', stableType, factory)),
        ...actors.map(([stableType, factory]) =>
          objectCapability('actor', stableType, factory))
      ].sort((left, right) => {
        const kindOrder = left.objectKind.localeCompare(right.objectKind);
        return kindOrder !== 0
          ? kindOrder
          : left.stableType.localeCompare(right.stableType);
      });
      const current = this.publishedMeshNodeDescriptors.get(meshName);
      const previousRevision = this.descriptorRevisionByMesh.get(meshName) ?? 0n;
      const descriptor = {
        meshName,
        rid: status.routingId,
        lifecycleGeneration: status.lifecycleGeneration,
        descriptorRevision: maxBigInt(previousRevision, current?.descriptorRevision ?? 0n) + 1n,
        // Core resolves port zero. The configured advertise host replaces only
        // the host component; peers still use Core's resolved port.
        endpoint: advertisedMeshEndpoint(
          status.localEndpoint,
          registration.router?.advertiseHost
        ),
        objectRole: effectiveObjectRole(registration),
        entrySpotId: this.entrySpotId(meshName, registration),
        placementWeight: this.placementWeight(meshName),
        populationCapacity: {
          actors: {
            active: current?.populationCapacity.actors.active ?? 0,
            reserved: current?.populationCapacity.actors.reserved ?? 0,
            limit: registration.actorLimit ?? 10_000
          },
          spots: {
            active: current?.populationCapacity.spots.active ?? 0,
            reserved: current?.populationCapacity.spots.reserved ?? 0,
            limit: registration.spotLimit ?? 128
          },
          spotTypes: capabilities
            .filter(capability => capability.objectKind !== 'actor')
            .map(capability => {
              const previous = current?.populationCapacity.spotTypes.find(candidate =>
                candidate.objectKind === capability.objectKind
                && candidate.stableType === capability.stableType);
              return {
                objectKind: capability.objectKind as 'user_spot' | 'instance_spot',
                stableType: capability.stableType,
                active: previous?.active ?? 0,
                reserved: previous?.reserved ?? 0,
                limit: capability.limit
              };
            })
        },
        activationConcurrency: {
          active: current?.activationConcurrency.active ?? 0,
          limit: registration.activationConcurrencyLimit ?? 128
        },
        channelWeights: Object.fromEntries(
          Object.entries(registration.meshChannels ?? {})
            .filter(([, channel]) => channel.server === true)
            .map(([channelName, channel]) => [
              channelName,
              state === ZLinkFrameworkRuntimeState.Draining
                ? 0
                : this.effectiveChannelWeight(meshName, channelName, channel.weight ?? 100)
            ])
        ),
        applicationVersion: this.options.registration.applicationVersion,
        maintenanceWave: this.options.registration.maintenanceWave,
        spotTypes: [
          ...userSpots.map(([stableType]) => stableType),
          ...instanceSpots.map(([stableType]) => stableType),
          ...(registration.entrySpotType === undefined
            ? []
            : [registration.entrySpotType.name])
        ]
          .sort(),
        objectCapabilities: capabilities,
        state,
        securityIdentity: 'default',
      };
      const result = await location.runtime.writeMeshNode(
        descriptor,
        ownerChanged
          ? ZLinkLocationWriteIntent.Takeover
          : current === undefined
            ? ZLinkLocationWriteIntent.NewClaim
            : ZLinkLocationWriteIntent.Renew,
        signal
      );
      if (result.status !== ZLinkLocationWriteStatus.Stored) {
        throw new ZLinkConfigurationException(
          `MeshNode '${meshName}' descriptor publication failed: ${result.status}.`
        );
      }
      this.publishedMeshNodeDescriptors.set(meshName, {
        ...descriptor,
        ownerId: location.runtime.currentOwnerToken!.ownerId,
        leaseGeneration: location.runtime.currentOwnerToken!.leaseGeneration,
        updatedAt: result.updatedAt
      });
      this.descriptorRevisionByMesh.set(meshName, descriptor.descriptorRevision);
    }
    this.publishedOwnerToken = owner;
  }

  async reconcileAndPublishMeshNodeState(
    state: ZLinkFrameworkRuntimeState,
    meshName: string,
    signal?: AbortSignal
  ): Promise<void> {
    const location = this.locationAutoConnect;
    const node = this.meshNodes.get(meshName);
    if (location === undefined || node === undefined) return;
    const status = node.status();
    const owner = location.runtime.currentOwnerToken;
    if (owner === undefined) return;
    const current = (await location.runtime.listLiveMeshNodes(meshName, signal))
      .find((descriptor) =>
        String(descriptor.rid) === String(status.routingId)
        && descriptor.lifecycleGeneration === status.lifecycleGeneration
        && descriptor.ownerId === owner.ownerId
        && descriptor.leaseGeneration === owner.leaseGeneration);
    if (current !== undefined) {
      this.publishedMeshNodeDescriptors.set(meshName, current);
      this.descriptorRevisionByMesh.set(
        meshName,
        maxBigInt(this.descriptorRevisionByMesh.get(meshName) ?? 0n, current.descriptorRevision)
      );
      this.publishedOwnerToken = owner;
    }
    await this.publishMeshNodeState(state, signal, meshName);
  }

  private entrySpotId(
    meshName: string,
    registration: ZLinkSpotNodeOptions
  ): string | undefined {
    if (effectiveObjectRole(registration) !== ZLinkObjectRole.Server
      || registration.entrySpotType === undefined) {
      return undefined;
    }
    let entrySpotId = this.entrySpotIds.get(meshName);
    if (entrySpotId !== undefined) return entrySpotId;
    const prefix = registration.routingIdPrefix ?? meshName;
    entrySpotId = createFrameworkEntrySpotId(prefix);
    this.entrySpotIds.set(meshName, entrySpotId);
    return entrySpotId;
  }

  get meshNodesByName(): ReadonlyMap<string, ZLinkBackendMeshNode> {
    return this.meshNodes;
  }

  meshNode(meshName: string): ZLinkBackendMeshNode | undefined {
    return this.meshNodes.get(meshName);
  }

  sendMessageFollowNotification(
    sourceNodeRid: string,
    targetNodeRid: string,
    record: Omit<ServiceMessageFollowRecord, 'kind'>
  ): boolean {
    for (const node of this.meshNodes.values()) {
      if (String(node.status().routingId) !== sourceNodeRid) continue;
      if (node.sendMessageFollowNotification === undefined) return false;
      node.sendMessageFollowNotification(targetNodeRid, record);
      return true;
    }
    return false;
  }

  entrySpotIdForMesh(meshName: string): string | undefined {
    const registration = this.options.registration.spotNodes.get(meshName);
    return registration === undefined
      ? undefined
      : this.entrySpotId(meshName, registration);
  }

  meshNodeDescriptor(meshName: string): ZLinkMeshNodeDescriptor | undefined {
    return this.publishedMeshNodeDescriptors.get(meshName);
  }

  placementWeight(meshName: string): number {
    const registration = this.options.registration.spotNodes.get(meshName);
    if (registration === undefined) {
      throw new ZLinkConfigurationException(`Mesh '${meshName}' is not registered.`);
    }
    return this.runtimePlacementWeights.get(meshName)
      ?? registration.placementWeight
      ?? 100;
  }

  channelWeight(channelName: string): number {
    const match = this.resolveChannel(channelName);
    return this.effectiveChannelWeight(match.meshName, channelName, match.configuredWeight);
  }

  setRuntimePlacementWeight(meshName: string, weight: number): void {
    const node = this.meshNodes.get(meshName);
    if (node === undefined) {
      throw new ZLinkConfigurationException(
        `Mesh '${meshName}' runtime placement weight requires a started MeshNode.`
      );
    }
    const validated = requirePublicRuntimeWeight(weight, 'Placement weight');
    if (this.placementWeight(meshName) === validated) return;
    node.setPlacementWeight(validated);
    this.runtimePlacementWeights.set(meshName, validated);
    this.scheduleRuntimeWeightPublication(meshName);
  }

  setRuntimeChannelWeight(channelName: string, weight: number): void {
    const match = this.resolveChannel(channelName);
    const node = this.meshNodes.get(match.meshName);
    if (node === undefined) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' runtime weight requires a started MeshNode.`
      );
    }
    const validated = requirePublicRuntimeWeight(weight, 'Channel weight');
    if (this.effectiveChannelWeight(match.meshName, channelName, match.configuredWeight) === validated) return;
    node.setChannelWeight(channelName, validated);
    let weights = this.runtimeChannelWeights.get(match.meshName);
    if (weights === undefined) {
      weights = new Map();
      this.runtimeChannelWeights.set(match.meshName, weights);
    }
    weights.set(channelName, validated);
    this.scheduleRuntimeWeightPublication(match.meshName);
  }

  waitForRuntimeWeightPublication(): Promise<void> {
    return this.runtimeWeightPublication;
  }

  meshCompletionTable(meshName: string): ZLinkMeshCompletionTable | undefined {
    return this.meshCompletions.get(meshName);
  }

  get primaryMeshNode(): ZLinkBackendMeshNode | undefined {
    return this.options.primaryMeshName === undefined
      ? this.meshNodes.values().next().value
      : this.meshNodes.get(this.options.primaryMeshName);
  }

  private effectiveChannelWeight(
    meshName: string,
    channelName: string,
    configuredWeight: number
  ): number {
    return this.runtimeChannelWeights.get(meshName)?.get(channelName) ?? configuredWeight;
  }

  private resolveChannel(channelName: string): {
    readonly meshName: string;
    readonly configuredWeight: number;
  } {
    const matches = [...this.options.registration.spotNodes]
      .flatMap(([meshName, registration]) => {
        const channel = registration.meshChannels?.[channelName];
        return channel === undefined
          ? []
          : [{ meshName, configuredWeight: channel.weight ?? 100 }];
      });
    if (matches.length === 0) {
      throw new ZLinkConfigurationException(`RouteMesh channel '${channelName}' is not registered.`);
    }
    if (matches.length > 1) {
      throw new ZLinkConfigurationException(
        `RouteMesh channel '${channelName}' is registered in more than one Mesh.`
      );
    }
    return matches[0]!;
  }

  private scheduleRuntimeWeightPublication(meshName: string): void {
    const publish = this.runtimeWeightPublication.catch(() => undefined).then(async () => {
      const state = this.publishedMeshNodeDescriptors.get(meshName)?.state
        ?? ZLinkFrameworkRuntimeState.Serving;
      await this.publishMeshNodeState(state, undefined, meshName);
    });
    this.runtimeWeightPublication = publish;
    if (this.options.detachedTaskRunner !== undefined) {
      this.options.detachedTaskRunner.runDetached(
        `RouteMesh '${meshName}' runtime weight publication`,
        async () => await publish
      );
    } else {
      void publish.catch(() => undefined);
    }
  }

  get primaryMeshCompletions(): ZLinkMeshCompletionTable | undefined {
    const meshName = this.options.primaryMeshName ?? this.meshNodes.keys().next().value;
    return meshName === undefined ? undefined : this.meshCompletions.get(meshName);
  }

  async notifyEntrySpotActorCreated(
    nodeRid: RoutingId,
    actor: ZLinkActor,
    createRequest: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<import('../../contracts').ZLinkActorCreateResponse | undefined> {
    let activation = [...this.entryActivations.values()].find(
      (entryActivation) => routingIdsEqual(entryActivation.nodeRid, nodeRid)
    );
    if (activation === undefined) {
      for (const [meshName, node] of this.meshNodes) {
        if (!routingIdsEqual(node.status().routingId, nodeRid)) continue;
        await this.ensureEntryActivation(meshName);
        activation = this.entryActivations.get(meshName);
        break;
      }
    }
    return activation?.notifyCreateActor(actor, createRequest, signal);
  }

  notifyPrimaryEntrySpotActorJoined(
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    const activation = this.primaryEntryActivation();
    return activation?.notifyJoinActor(actor, signal) ?? Promise.resolve();
  }

  notifyPrimaryEntrySpotActorLeft(
    actor: ZLinkActor,
    signal?: AbortSignal,
    actorRef?: ActorRef,
    membershipEpoch?: bigint
  ): Promise<void> {
    const activation = this.primaryEntryActivation();
    return activation?.notifyLeaveActor(actor, signal, actorRef, membershipEpoch) ?? Promise.resolve();
  }

  notifyPrimaryEntrySpotActorDisconnected(
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    const activation = this.primaryEntryActivation();
    return activation?.notifyDisconnectActor(actor, signal) ?? Promise.resolve();
  }

  async notifyEntrySpotActorDisconnected(
    meshName: string,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    await this.ensureEntryActivation(meshName);
    await this.entryActivations.get(meshName)?.notifyDisconnectActor(actor, signal);
  }

  async dispose(signal?: AbortSignal): Promise<void> {
    this.disposed = true;
    const autoConnectLoops = [...this.autoConnectLoops];
    const entryActivations = [...this.entryActivations.values()];
    const meshPumps = [...this.meshPumps.values()];
    const meshNodes = [...this.meshNodes.values()];
    const meshCompletions = [...this.meshCompletions.values()];
    this.entryActivations.clear();
    this.autoConnectLoops.length = 0;
    this.meshPumps.clear();
    const publishers = [...this.publishers.values()];
    this.publishers.clear();
    this.rejectPublishSlotWaiters(runtimeShutdownError());
    const errors: unknown[] = [];
    const settle = async (operations: readonly Promise<unknown>[]) => {
      const results = await Promise.allSettled(operations);
      errors.push(...results
        .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
        .map((result) => result.reason));
    };
    await settle(publishers.map(async (publisher) => publisher.close()));
    await settle(autoConnectLoops.map((loop) => loop.prepareTransportShutdown()));
    await settle(entryActivations.reverse().map((activation) => activation.dispose()));
    await settle(meshPumps.reverse().map((pump) => pump.dispose()));
    await settle(meshNodes.reverse().map(async (node) => {
      node.shutdown(1000);
      node.close();
    }));
    for (const completions of meshCompletions) {
      completions.dispose();
    }
    this.meshNodes.clear();
    this.meshCompletions.clear();
    await settle(autoConnectLoops.map((loop) => loop.finishTransportShutdown(signal)));
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'SPOT node runtime cleanup failed.');
  }

  private async dispatchMeshRecord(
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord
  ): Promise<void> {
    if (record.kind === ReceiveKind.Completion) {
      const completions = this.meshCompletions.get(meshName);
      if (completions === undefined) {
        throw new ZLinkConfigurationException(
          `MeshNode '${meshName}' received a completion before its completion table was registered.`
        );
      }
      completions.complete(record);
      return;
    }
    const node = this.meshNodes.get(meshName);
    const targetsEntrySpot = owner.spotId === null
      || (node !== undefined && routingIdsEqual(
        owner.spotId as unknown as RoutingId,
        String(node.status().routingId)
      ));
    if (targetsEntrySpot && (
      record.kind === ReceiveKind.ActorSend
      || record.kind === ReceiveKind.ActorRequest
      || record.kind === ReceiveKind.SpotSend
      || record.kind === ReceiveKind.SpotRequest
      || record.kind === ReceiveKind.SpotMulticast
    )) {
      await this.ensureEntryActivation(meshName);
    }
    const dispatcher = this.options.meshRecordDispatcher;
    if (dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `MeshNode '${meshName}' received a record before its framework dispatcher was registered.`
      );
    }
    await dispatcher(meshName, owner, record);
  }

  async dispatchEntrySpotRecord(
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord
  ): Promise<boolean> {
    if (record.kind !== ReceiveKind.SpotSend
      && record.kind !== ReceiveKind.SpotRequest
      && record.kind !== ReceiveKind.SpotMulticast) {
      return false;
    }
    await this.ensureEntryActivation(meshName);
    const activation = this.entryActivations.get(meshName);
    if (
      activation === undefined
      || owner.spotId === null
      || activation.spotId !== owner.spotId
    ) {
      return false;
    }
    if (record.kind === ReceiveKind.SpotMulticast) {
      if (record.topic === null || record.topic.length === 0) {
        throw new ZLinkConfigurationException('MeshNode Entry Spot multicast record is missing its topic.');
      }
      await activation.dispatchSubscriptionRecord(
        record.topic,
        record.parts,
        record.sourceNodeRid as unknown as RoutingId | null
      );
      return true;
    }
    const envelope = decodeChannelEnvelope(record.parts);
    const codecs = { serializers: this.options.registration.messageSerializers };
    const decodePayload = () => decodeChannelPayload(envelope, codecs);
    const context = {
      channelName: envelope.header.channelName,
      contentType: envelope.header.contentType
    };
    if (record.kind === ReceiveKind.SpotSend) {
      await activation.dispatchPacketEncoded(envelope.packetName, decodePayload, context, false);
      return true;
    }
    try {
      const response = await activation.dispatchPacketEncoded(
        envelope.packetName,
        decodePayload,
        context,
        true
      );
      requireEntrySpotReply(record.reply(encodeChannelReplyParts(envelope.header, response, codecs)));
    } catch (error) {
      requireEntrySpotReply(record.reply(encodeChannelErrorReplyParts(envelope.header, error)));
    }
    return true;
  }

  private async ensureEntryActivation(spotNodeName: string): Promise<void> {
    if (this.entryActivations.has(spotNodeName)) {
      return;
    }
    const pending = this.entryActivationStarts.get(spotNodeName);
    if (pending !== undefined) {
      await pending;
      return;
    }
    const spotNode = this.options.registration.spotNodes.get(spotNodeName);
    const node = this.meshNodes.get(spotNodeName);
    if (spotNode === undefined || node === undefined || spotNode.entrySpotType === undefined) {
      return;
    }
    const start = this.initializeEntrySpot(spotNodeName, node, spotNode);
    this.entryActivationStarts.set(spotNodeName, start);
    try {
      await start;
    } finally {
      if (this.entryActivationStarts.get(spotNodeName) === start) {
        this.entryActivationStarts.delete(spotNodeName);
      }
    }
  }

  private async initializeEntrySpot(
    spotNodeName: string,
    node: ZLinkBackendMeshNode,
    spotNode: ZLinkSpotNodeOptions
  ): Promise<void> {
    if (spotNode.entrySpotType === undefined) {
      return;
    }
    const activation = new ZLinkEntrySpotActivation({
      entrySpotType: spotNode.entrySpotType,
      nativeSpot: node.entrySpot() as never,
      nativeNode: node as unknown as ZLinkBackendSpotNode,
      createReceived: () => this.options.backendAdapterFactory.createReceived(),
      createTopicMessage: () => this.options.backendAdapterFactory.createTopicMessage(),
      nodeRid: String(node.status().routingId),
      spotNodeName,
      providerResolver: this.options.providerResolver,
      channelClient: this.options.channelClient,
      fanoutClient: this.options.fanoutClient,
      spotPublisherClient: this.options.spotPublisherClient,
      routedTransport: this.options.routedTransport,
      spotRouterChannelIdForMesh: this.options.spotRouterChannelIdForMesh,
      channelMeshNameForChannel: (channelName) => resolveChannelMeshName(
        this.options.registration,
        channelName
      ),
      timerHandlers: spotNode.entrySpotTimerHandlers,
      packetHandlers: spotNode.entrySpotPacketHandlers,
      subscriptionHandlers: spotNode.entrySpotSubscriptionHandlers,
      actorSendHandlers: spotNode.entrySpotActorSendHandlers,
      actorRequestHandlers: spotNode.entrySpotActorRequestHandlers,
      messageSerializers: this.options.registration.messageSerializers,
      entryActorRuntime: this.options.entryActorRuntime,
      actorTransferRuntime: this.options.actorTransferRuntime,
      boundSessionRuntime: this.options.boundSessionRuntime,
      actorHandoffRuntime: this.options.actorHandoffRuntime,
      detachedTaskRunner: this.options.detachedTaskRunner,
      dispatchErrors: this.options.dispatchErrors,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      metrics: this.options.metrics
    });
    try {
      await activation.create();
      await activation.configure();
      await activation.initialize();
      this.entryActivations.set(spotNodeName, activation);
    } catch (error) {
      try {
        await activation.dispose();
      } catch (disposeError) {
        throw new AggregateError(
          [error, disposeError],
          `Entry Spot '${spotNodeName}' materialization cleanup failed.`
        );
      }
      throw error;
    }
  }

  private primaryEntryActivation(): ZLinkEntrySpotActivation | undefined {
    return this.entryActivations.values().next().value;
  }

  async dispatchEntryActorPacket(
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    let activation = this.primaryEntryActivation();
    if (activation === undefined) {
      const candidates = this.options.primaryMeshName === undefined
        ? this.options.registration.spotNodes
        : new Map([[this.options.primaryMeshName, this.options.registration.spotNodes.get(
          this.options.primaryMeshName
        )]]);
      for (const [spotNodeName, spotNode] of candidates) {
        if (spotNode === undefined) {
          continue;
        }
        if (spotNode.entrySpotType === undefined) {
          continue;
        }
        await this.ensureEntryActivation(spotNodeName);
        activation = this.entryActivations.get(spotNodeName);
        if (activation !== undefined) {
          break;
        }
      }
    }
    if (activation === undefined) {
      throw new ZLinkConfigurationException('Entry Spot actor packet dispatch requires an Entry Spot.');
    }
    return await activation.dispatchActorPacket(
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal,
      messageFollowOrigin
    );
  }

  async dispatchEntryActorJoin(
    meshName: string,
    actor: ZLinkActor,
    handoffBacklog: readonly ZLinkActorHandoffPacket[] = []
  ): Promise<void> {
    await this.ensureEntryActivation(meshName);
    const activation = this.entryActivations.get(meshName);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(
        `Entry Spot actor join requires an Entry Spot for MeshNode '${meshName}'.`
      );
    }
    await activation.commitServiceActorJoin(actor, handoffBacklog);
  }

  publish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    signal?: AbortSignal,
    metadata: ReadonlyMap<string, string> = new Map()
  ): Promise<ZLinkSubmitResult> {
    if (this.disposed) {
      return Promise.reject(runtimeShutdownError());
    }
    if (signal?.aborted === true) {
      return Promise.reject(signal.reason ?? createAbortError());
    }
    const publisher = this.publishers.get(meshName);
    if (publisher === undefined) {
      throw new ZLinkConfigurationException(`RouteMesh '${meshName}' publisher is not started.`);
    }
    const sendTimeoutMs = this.options.registration.spotNodes
      .get(meshName)?.publisherConfig?.sendTimeoutMs ?? 1000;
    const slot = this.acquirePublishSlot(meshName, sendTimeoutMs, signal);
    if (slot === false) {
      // The bounded executor-admission tokens are exhausted. Do not retain the
      // payload, cancellation state, or a timer for this hard-overload call.
      return Promise.resolve(this.publishTimedOut());
    }
    if (slot === true) {
      return this.executePublish(
        publisher,
        meshName,
        channelName,
        topic,
        packetName,
        event,
        metadata
      );
    }
    return slot.then((acquired) => acquired
      ? this.executePublish(
        publisher,
        meshName,
        channelName,
        topic,
        packetName,
        event,
        metadata
      )
      : this.publishTimedOut());
  }

  private async executePublish(
    publisher: ReturnType<ZLinkBackendMeshNode['createPublisher']>,
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    metadata: ReadonlyMap<string, string>
  ): Promise<ZLinkSubmitResult> {
    // The slot is the source-local admission boundary. Once publishAsync takes
    // the owned frames, target processing cannot change the caller terminal.
    try {
      const parts = encodeChannelPublishEnvelopeParts(
        channelName,
        topic,
        packetName,
        event,
        undefined,
        this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true,
        metadata
      );
      const processing = publisher.publishAsync(
        channelName,
        topic,
        parts,
        undefined,
        undefined
      );
      void Promise.resolve(processing).catch(() => undefined);
      return Promise.resolve({ status: ZLinkSubmitStatus.Submitted });
    } finally {
      this.releasePublishSlot(meshName);
    }
  }

  tryPublish(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    metadata: ReadonlyMap<string, string> = new Map()
  ): ZLinkSubmitResult {
    return this.publishWithFlags(meshName, channelName, topic, packetName, event, ZLINK_SEND_DONT_WAIT, metadata);
  }

  private publishWithFlags(
    meshName: string,
    channelName: string,
    topic: string,
    packetName: string | undefined,
    event: Message,
    flags: number,
    metadata: ReadonlyMap<string, string>
  ): ZLinkSubmitResult {
    if (this.disposed) {
      throw runtimeShutdownError();
    }
    const publisher = this.publishers.get(meshName);
    if (publisher === undefined) {
      throw new ZLinkConfigurationException(`RouteMesh '${meshName}' publisher is not started.`);
    }
    const parts = encodeChannelPublishEnvelopeParts(
      channelName,
      topic,
      packetName,
      event,
      undefined,
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true,
      metadata
    );
    try {
      publisher.publish(channelName, topic, parts, { flags });
      return { status: ZLinkSubmitStatus.Submitted };
    } catch (error) {
      if (isZLinkBackendResultError(error) && error.operation === 'submit') {
        return { status: mapPublishSubmitStatus(error.result) };
      }
      throw error;
    }
  }

  private acquirePublishSlot(
    meshName: string,
    timeoutMs: number,
    signal?: AbortSignal
  ): boolean | Promise<boolean> {
    if (!this.activePublishes.has(meshName)) {
      this.activePublishes.add(meshName);
      return true;
    }
    const waiterCapacity = Math.max(
      1,
      this.options.registration.spotNodes
        .get(meshName)?.publisherConfig?.sendHighWaterMark ?? 1
    );
    const queue = this.publishSlotWaiters.get(meshName) ?? {
      waiters: [],
      head: 0,
      count: 0
    };
    if (queue.count >= waiterCapacity) {
      // This call has no bounded executor-admission token. Fail without
      // retaining its payload in a pending work queue.
      return false;
    }
    return new Promise<boolean>((resolve, reject) => {
      const waiter: ZLinkPublishSlotWaiter = {
        resolve,
        reject,
        signal,
        settled: false
      };
      if (timeoutMs !== -1) {
        waiter.deadlineMs = Date.now() + Math.max(0, timeoutMs);
      }
      queue.waiters.push(waiter);
      queue.count += 1;
      this.publishSlotWaiters.set(meshName, queue);
      if (timeoutMs !== -1) {
        waiter.timeout = setTimeout(() => this.settlePublishSlotWaiter(
          meshName,
          waiter,
          false
        ), Math.max(0, timeoutMs));
      }
      if (signal !== undefined) {
        waiter.abortHandler = () => this.rejectPublishSlotWaiter(
          meshName,
          waiter,
          signal.reason ?? createAbortError()
        );
        signal.addEventListener('abort', waiter.abortHandler, { once: true });
      }
    });
  }

  private publishTimedOut(): ZLinkSubmitResult {
    return { status: ZLinkSubmitStatus.TimedOut };
  }

  private releasePublishSlot(meshName: string): void {
    const queue = this.publishSlotWaiters.get(meshName);
    while (queue !== undefined && queue.count > 0) {
      const waiter = this.takePublishSlotWaiter(queue);
      if (waiter === undefined) continue;
      if (waiter.settled) continue;
      if (waiter.deadlineMs !== undefined && Date.now() >= waiter.deadlineMs) {
        this.cleanupPublishSlotWaiter(waiter);
        waiter.settled = true;
        waiter.resolve(false);
        continue;
      }
      this.cleanupPublishSlotWaiter(waiter);
      waiter.settled = true;
      waiter.resolve(true);
      if (queue.count === 0) this.publishSlotWaiters.delete(meshName);
      return;
    }
    this.publishSlotWaiters.delete(meshName);
    this.activePublishes.delete(meshName);
  }

  private settlePublishSlotWaiter(
    meshName: string,
    waiter: ZLinkPublishSlotWaiter,
    acquired: boolean
  ): void {
    if (waiter.settled) return;
    waiter.settled = true;
    this.removePublishSlotWaiter(meshName, waiter);
    this.cleanupPublishSlotWaiter(waiter);
    waiter.resolve(acquired);
  }

  private rejectPublishSlotWaiter(
    meshName: string,
    waiter: ZLinkPublishSlotWaiter,
    error: unknown
  ): void {
    if (waiter.settled) return;
    waiter.settled = true;
    this.removePublishSlotWaiter(meshName, waiter);
    this.cleanupPublishSlotWaiter(waiter);
    waiter.reject(error);
  }

  private removePublishSlotWaiter(meshName: string, waiter: ZLinkPublishSlotWaiter): void {
    const queue = this.publishSlotWaiters.get(meshName);
    if (queue === undefined) return;
    const index = queue.waiters.indexOf(waiter, queue.head);
    if (index >= 0) {
      queue.waiters[index] = undefined;
      queue.count -= 1;
      this.compactPublishSlotWaiters(queue);
    }
    if (queue.count === 0) this.publishSlotWaiters.delete(meshName);
  }

  private cleanupPublishSlotWaiter(waiter: ZLinkPublishSlotWaiter): void {
    if (waiter.timeout !== undefined) clearTimeout(waiter.timeout);
    if (waiter.abortHandler !== undefined) {
      waiter.signal?.removeEventListener('abort', waiter.abortHandler);
    }
  }

  private rejectPublishSlotWaiters(error: unknown): void {
    for (const [meshName, queue] of this.publishSlotWaiters) {
      let waiter: ZLinkPublishSlotWaiter | undefined;
      while ((waiter = this.takePublishSlotWaiter(queue)) !== undefined) {
        this.rejectPublishSlotWaiter(meshName, waiter, error);
      }
      this.publishSlotWaiters.delete(meshName);
    }
    this.activePublishes.clear();
  }

  private takePublishSlotWaiter(queue: ZLinkPublishSlotQueue): ZLinkPublishSlotWaiter | undefined {
    while (queue.head < queue.waiters.length && queue.waiters[queue.head] === undefined) {
      queue.head += 1;
    }
    const waiter = queue.waiters[queue.head];
    if (waiter === undefined) return undefined;
    queue.waiters[queue.head] = undefined;
    queue.head += 1;
    queue.count -= 1;
    this.compactPublishSlotWaiters(queue);
    return waiter;
  }

  private compactPublishSlotWaiters(queue: ZLinkPublishSlotQueue): void {
    if (queue.count === 0) {
      queue.waiters.length = 0;
      queue.head = 0;
    } else if (queue.head >= 1024 && queue.head * 2 >= queue.waiters.length) {
      queue.waiters.splice(0, queue.head);
      queue.head = 0;
    }
  }
}

function advertisedMeshEndpoint(
  boundEndpoint: string,
  advertiseHost: string | undefined
): string {
  if (advertiseHost === undefined) return boundEndpoint;
  const match = /^tcp:\/\/(?:\[[^\]]+\]|[^:]+):(\d+)$/.exec(boundEndpoint);
  if (match === null) {
    throw new ZLinkConfigurationException(
      `RouteMesh advertise host requires a TCP endpoint, received '${boundEndpoint}'.`
    );
  }
  const host = advertiseHost.includes(':') && !advertiseHost.startsWith('[')
    ? `[${advertiseHost}]`
    : advertiseHost;
  return `tcp://${host}:${match[1]}`;
}

function runtimeShutdownError(): ZLinkFrameworkException {
  return createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
    'SPOT publisher runtime is shutting down.'
  );
}

export function createFrameworkEntrySpotId(prefix: string): string {
  if (!/^[A-Za-z0-9._-]{1,64}$/.test(prefix)) {
    throw new ZLinkConfigurationException(
      'Entry Spot diagnostic prefix must contain 1..64 ASCII letters, digits, dot, underscore, or hyphen.'
    );
  }
  return `${prefix}-entry-${randomUUID()}`;
}

function resolveChannelMeshName(
  registration: ZLinkFrameworkRegistration,
  channelName: string
): string | undefined {
  const matches = [...registration.spotNodes.entries()]
    .filter(([, node]) => Object.prototype.hasOwnProperty.call(node.meshChannels ?? {}, channelName))
    .map(([meshName]) => meshName);
  return matches.length === 1 ? matches[0] : undefined;
}

function mapPublishSubmitStatus(result: number): ZLinkSubmitStatus {
  switch (result) {
    case SubmitResult.Ok:
      return ZLinkSubmitStatus.Submitted;
    case SubmitResult.Backpressured:
    case SubmitResult.NotAdmitted:
      return ZLinkSubmitStatus.Backpressured;
    case SubmitResult.NotFound:
      // A publish with no matching subscriber is a successful zero-recipient
      // operation, not an operation-specific not-found failure.
      return ZLinkSubmitStatus.Submitted;
    case SubmitResult.NotConnected:
      return ZLinkSubmitStatus.RouteNotConnected;
    case SubmitResult.Terminated:
    case SubmitResult.InvalidHandle:
      return ZLinkSubmitStatus.Shutdown;
    default:
      throw new ZLinkConfigurationException(`Logical Multicast failed with submit result '${result}'.`);
  }
}

function requireEntrySpotReply(result: number): void {
  if (result !== SubmitResult.Ok) {
    throw new ZLinkConfigurationException(
      `MeshNode Entry Spot reply was not accepted (submit result ${result}).`
    );
  }
}

function requirePublicRuntimeWeight(value: number, label: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new ZLinkConfigurationException(
      `${label} must be an integer in 0..10000.`
    );
  }
  return value;
}

interface DescriptorFactoryRegistration {
  readonly options?: {
    readonly stableTypeLimit?: number;
  };
  readonly relocation?: {
    readonly kind: 'disabled' | 'recreate' | 'snapshot';
  };
}

function effectiveUserSpotRegistrations(
  registration: ZLinkSpotNodeOptions
): Array<[string, DescriptorFactoryRegistration]> {
  const registrations = new Map<string, DescriptorFactoryRegistration>(
    Object.entries(registration.spotFactoryRegistrations ?? {})
  );
  const explicitImplementations = new Set(
    Object.values(registration.spotFactoryRegistrations ?? {})
      .map(({ implementation }) => implementation)
  );
  for (const implementation of registration.spotFactories ?? []) {
    // `spotFactories` is also retained as the runtime implementation set.
    // When the caller supplied an explicit stable type, the implementation
    // must not be projected a second time under its JavaScript class name.
    // That legacy alias advertises a different public placement type and can
    // make location routing disagree with the configured factory.
    if (!explicitImplementations.has(implementation) && !registrations.has(implementation.name)) {
      registrations.set(implementation.name, {});
    }
  }
  return [...registrations];
}

function effectiveInstanceSpotRegistrations(
  registration: ZLinkSpotNodeOptions
): Array<[string, DescriptorFactoryRegistration]> {
  const registrations = new Map<string, DescriptorFactoryRegistration>(
    Object.entries(registration.instanceSpotFactoryRegistrations ?? {})
  );
  for (const stableType of Object.keys(registration.instanceSpotFactories ?? {})) {
    if (!registrations.has(stableType)) registrations.set(stableType, {});
  }
  return [...registrations];
}

function effectiveActorRegistrations(
  registration: ZLinkSpotNodeOptions
): Array<[string, DescriptorFactoryRegistration]> {
  const registrations = new Map<string, DescriptorFactoryRegistration>(
    Object.entries(registration.actorFactoryRegistrations ?? {})
  );
  const legacy = registration.actorFactories instanceof Map
    ? registration.actorFactories.keys()
    : Object.keys(registration.actorFactories ?? {});
  for (const stableType of legacy) {
    if (!registrations.has(stableType)) registrations.set(stableType, {});
  }
  return [...registrations];
}

function effectiveObjectRole(
  registration: ZLinkSpotNodeOptions
): ZLinkObjectRole {
  if (registration.objectRole === 'server') return ZLinkObjectRole.Server;
  if (registration.objectRole === 'client') return ZLinkObjectRole.Client;
  return effectiveUserSpotRegistrations(registration).length > 0
    || effectiveInstanceSpotRegistrations(registration).length > 0
    || effectiveActorRegistrations(registration).length > 0
    ? ZLinkObjectRole.Server
    : ZLinkObjectRole.None;
}

function objectCapability(
  objectKind: 'actor' | 'user_spot' | 'instance_spot',
  stableType: string,
  factory: DescriptorFactoryRegistration
) {
  const policy = factory.relocation?.kind ?? 'disabled';
  return {
    objectKind,
    stableType,
    policy,
    hasSnapshotAdapter: policy === 'snapshot',
    limit: objectKind === 'actor'
      ? 0
      : factory.options?.stableTypeLimit ?? 0
  };
}

function maxBigInt(left: bigint, right: bigint): bigint {
  return left > right ? left : right;
}

function sameOwnerToken(
  left: ZLinkLocationOwnerToken,
  right: ZLinkLocationOwnerToken
): boolean {
  return left.ownerId === right.ownerId
    && left.leaseGeneration === right.leaseGeneration;
}
