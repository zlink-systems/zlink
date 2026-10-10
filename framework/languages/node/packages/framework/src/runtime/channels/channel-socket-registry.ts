import { RuntimeDisposal } from '../disposal';
import { UINT64_MAX } from '@zlink-systems/stream-wire';
import { randomBytes, randomUUID } from 'node:crypto';
import {
  ZLinkFrameworkRuntimeState,
  type RoutingId,
  type ZLinkClientServerServerDescriptor,
  type ZLinkFanoutPublisherDescriptor
} from '../../contracts';
import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import type { Message } from '../../contracts/Common/Message';
import {
  isValidPublicWeight,
  ZLINK_DEFAULT_PUBLIC_WEIGHT,
  ZLINK_MAX_PUBLIC_WEIGHT
} from '../../contracts/Configuration/RegistrationBuilderPolicy';
import type { ZLinkChannelOptions } from '../../contracts/Configuration/RegistrationTypes';
import { attachEndpointConnections } from '../../contracts/Configuration/RuntimeEndpointConnections';
import { createAbortError, throwIfAborted } from '../abort';
import type {
  ZLinkBackendContext,
  ZLinkBackendDealerSocket,
  ZLinkBackendPublisherSocket,
  ZLinkBackendReadablePoller,
  ZLinkBackendRouterSocket,
  ZLinkBackendSocketMonitor,
  ZLinkBackendSocketMonitorEvent,
  ZLinkBackendSubscriberSocket,
  ZLinkChannelBackendAdapter,
  ZLinkMonitoringBackendAdapter
} from '../backend/contracts';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { isBackendRequestTimeoutError } from '../backend/runtime-values';
import {
  buildAdvertisedEndpoint,
  ZLinkConfigurationException,
  type ZLinkFrameworkRegistration
} from '../configuration';
import { ZLinkSocketNativeEventType } from '../diagnostics/internal-event-contracts';
import { ZLinkListenerRecords } from '../foundation/listener-records';
import { discoveryAvailabilityForRuntimeState } from '../foundation/runtime-state-projections';
import { ServiceDiscoveryRegistry } from '../foundation/service-discovery-registry';
import {
  ServiceLivenessConnection,
  DEFAULT_SERVICE_PEER_TIMEOUT_MS,
  DEFAULT_SERVICE_PROBE_INTERVAL_MS
} from '../foundation/service-liveness-registry';
import { ServiceWireProtocolError } from '../foundation/service-wire-m6a-codec';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import type { ApplicationJobQueue } from '../host/application-job-queue';
import type { ZLinkChannelEnvelopeHeader } from './channel-envelope';
import {
  ClientServerRejectReason,
  decodeClientServerControl,
  encodeClientServerAdmit,
  encodeClientServerHello,
  encodeClientServerLivenessAck,
  encodeClientServerLivenessProbe,
  encodeClientServerReject,
  encodeClientServerUpdate,
  isClientServerControlFrame,
  normalizeClientServerMessageLimit,
  type ZLinkClientServerAdmission
} from './client-server-service-wire';
import {
  FANOUT_LIVENESS_PAYLOAD,
  FANOUT_LIVENESS_TOPIC,
  inspectFanoutInbound
} from './fanout-service-wire';
import { ZLinkRouteMemberSnapshot } from './route-member-snapshot';

const MAX_LIFECYCLE_GENERATION = 0x7fff_ffff_ffff_ffffn;
const CLIENT_SERVER_PROBE_INTERVAL_MS = DEFAULT_SERVICE_PROBE_INTERVAL_MS;
const CLIENT_SERVER_PEER_DEADLINE_MS = DEFAULT_SERVICE_PEER_TIMEOUT_MS;
const CLIENT_SERVER_LIVENESS_TICK_MS = 100;
const DEFAULT_SEND_TIMEOUT_MS = 1_000;

export interface ZLinkClientServerServerSocketIdentity {
  readonly serverRid: string;
  readonly lifecycleGeneration: bigint;
  readonly endpoint: string;
}

export interface ZLinkClientServerConnectionCallbacks {
  readonly onTransportReady: (routingId: string, endpoint: string) => void;
  readonly onTerminated: (routingId: string | undefined, endpoint: string) => void;
}

type ClientServerDiscoveryDescriptor = Parameters<ServiceDiscoveryRegistry['admitClientServer']>[0];

interface ClientServerPhysicalConnection {
  readonly channelName: string;
  readonly endpoint: string;
  readonly dealer: ZLinkBackendDealerSocket;
  readablePoller?: ZLinkBackendReadablePoller;
  monitor?: ZLinkBackendSocketMonitor;
  readonly aliases: Set<string>;
  readonly callbacksByAlias: Map<string, ZLinkClientServerConnectionCallbacks>;
  physicalConnectionId: symbol;
  readyConnectionId?: string;
  admittedDescriptor?: ClientServerDiscoveryDescriptor;
  liveness?: ServiceLivenessConnection;
  admissionAttempt?: symbol;
}

export interface ZLinkFanoutConnectionCallbacks {
  readonly onReady: () => void;
  readonly onTerminated: (reason: 'disconnect' | 'deadline' | 'protocol') => void;
}

interface ClientServerServerPeer {
  readonly channelName: string;
  readonly routingId: RoutingId;
  readonly normalizedEffectiveMaxMessageBytes: number;
  readonly liveness: ServiceLivenessConnection;
}

interface FanoutPublisherConnection {
  readonly channelName: string;
  readonly endpoint: string;
  readonly subscriber: ZLinkBackendSubscriberSocket;
  monitor?: ZLinkBackendSocketMonitor;
  readonly callbacks: ZLinkFanoutConnectionCallbacks;
  readonly physicalConnectionId: symbol;
  transportReady: boolean;
  ready: boolean;
  deadlineAt?: number;
}

type ReceiveFlowSocket = ZLinkBackendDealerSocket | ZLinkBackendRouterSocket;

export class ZLinkChannelSocketRegistry {
  private readonly clientDealers = new Map<string, ZLinkBackendDealerSocket>();
  private readonly channelRouters = new Map<string, ZLinkBackendRouterSocket>();
  private readonly publishers = new Map<string, ZLinkBackendPublisherSocket>();
  private readonly routeRouters = new Map<string, ZLinkBackendRouterSocket>();
  private readonly routeMembers = new ZLinkRouteMemberSnapshot();
  private readonly clientServerIdentities = new Map<
    string,
    {
      readonly serverRid: string;
      readonly lifecycleGeneration: bigint;
    }
  >();
  private readonly clientServerConnections = new Map<string, ClientServerPhysicalConnection>();
  private readonly clientServerDiscovery = new ServiceDiscoveryRegistry();
  private readonly clientServerReadyIdentities = new Map<
    string,
    {
      readonly channelName: string;
      readonly serverRoutingId: string;
      readonly lifecycleGeneration: bigint;
    }
  >();
  private readonly clientServerServerDescriptors = new Map<
    string,
    ZLinkClientServerServerDescriptor
  >();
  private readonly clientServerPublicWeights = new Map<string, number>();
  private readonly routeMeshPublicWeights = new Map<string, number>();
  private readonly clientServerServerPeers = new Map<
    string,
    Map<RoutingId, ClientServerServerPeer>
  >();
  private readonly disposal = new RuntimeDisposal();
  private readonly ownedResources = new Set<
    | ZLinkBackendSocketMonitor
    | ZLinkBackendReadablePoller
    | ZLinkBackendDealerSocket
    | ZLinkBackendSubscriberSocket
  >();
  private readonly fanoutConnections = new Map<string, FanoutPublisherConnection>();
  private readonly fanoutPublisherNextBeacon = new Map<string, number>();
  private clientServerLivenessTimer?: NodeJS.Timeout;
  private nextClientServerProbeId = 1n;
  private readonly clientServerMonitorHandlers = new Map<
    string,
    Set<(event?: ZLinkBackendSocketMonitorEvent) => void>
  >();
  private readonly fanoutMonitorHandlers = new Map<
    string,
    Set<(event: ZLinkBackendSocketMonitorEvent) => void>
  >();
  private readonly fanoutTopologyHandlers = new Map<string, Set<() => void>>();

  private readonly listenerRecords: ZLinkListenerRecords;

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly adapter: ZLinkChannelBackendAdapter,
    private readonly context: ZLinkBackendContext,
    private readonly monitoringAdapter?: ZLinkMonitoringBackendAdapter,
    private readonly oneWayFailureSink?: (error: unknown) => void,
    private readonly applicationJobQueue?: ApplicationJobQueue,
    listenerRecords?: ZLinkListenerRecords
  ) {
    this.listenerRecords = listenerRecords ?? new ZLinkListenerRecords();
  }

  dispose(): Promise<void> {
    return this.disposal.run(() => this.disposeCore());
  }

  private async disposeCore(): Promise<void> {
    this.clientServerDiscovery.dispose(
      createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
        'Channel sockets are shutting down.'
      )
    );
    const clientServerConnections = [...new Set(this.clientServerConnections.values())];
    const clientServerPollers = clientServerConnections.flatMap((connection) =>
      connection.readablePoller === undefined ? [] : [connection.readablePoller]
    );
    const sockets = [
      ...this.clientDealers.values(),
      ...clientServerConnections.map((value) => value.dealer),
      ...[...this.fanoutConnections.values()].map((value) => value.subscriber),
      ...this.channelRouters.values(),
      ...this.publishers.values(),
      ...this.routeRouters.values()
    ];
    for (const socket of new Set<ReceiveFlowSocket>([
      ...this.clientDealers.values(),
      ...clientServerConnections.map((value) => value.dealer),
      ...this.channelRouters.values(),
      ...this.routeRouters.values()
    ]))
      this.unregisterReceiveFlowSocket(socket);
    this.clientServerReadyIdentities.clear();
    this.clientServerServerDescriptors.clear();
    this.clientServerPublicWeights.clear();
    this.routeMeshPublicWeights.clear();
    for (const peers of this.clientServerServerPeers.values()) peers.clear();
    this.clientServerServerPeers.clear();
    this.clientServerMonitorHandlers.clear();
    this.fanoutMonitorHandlers.clear();
    this.fanoutTopologyHandlers.clear();
    this.fanoutPublisherNextBeacon.clear();
    if (this.clientServerLivenessTimer !== undefined) {
      clearInterval(this.clientServerLivenessTimer);
      this.clientServerLivenessTimer = undefined;
    }
    this.routeMembers.clear();
    const cleanup = await Promise.allSettled(
      [...new Set([...clientServerPollers, ...this.ownedResources, ...sockets])].map(
        async (resource) => {
          await resource.dispose();
          this.ownedResources.delete(resource as ZLinkBackendSocketMonitor);
          for (const owners of [
            this.clientDealers,
            this.channelRouters,
            this.publishers,
            this.routeRouters
          ]) {
            for (const [name, owned] of owners) if (owned === resource) owners.delete(name);
          }
          for (const [name, owned] of this.clientServerConnections) {
            if (owned.dealer === resource) this.clientServerConnections.delete(name);
            if (owned.readablePoller === resource) owned.readablePoller = undefined;
            if (owned.monitor === resource) owned.monitor = undefined;
          }
          for (const [name, owned] of this.fanoutConnections) {
            if (owned.subscriber === resource) this.fanoutConnections.delete(name);
            if (owned.monitor === resource) owned.monitor = undefined;
          }
        }
      )
    );
    const errors = cleanup
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Channel socket cleanup failed.');
  }

  clientDealer(channelName: string): ZLinkBackendDealerSocket {
    const existing = this.clientDealers.get(channelName);
    if (existing !== undefined) {
      return existing;
    }

    const channel = this.registration.channels.get(channelName);
    const client = channel?.client;
    if (client === undefined) {
      throw new ZLinkConfigurationException(`Channel client '${channelName}' is not registered.`);
    }

    const dealer = this.adapter.createDealerSocket(this.context);
    this.clientDealers.set(channelName, dealer);
    this.registerReceiveFlowSocket(dealer);
    dealer.setChannelName(channelName);
    if (channel?.routingId !== undefined && channel.routingId.length > 0) {
      dealer.setRoutingId(deriveRoutingId(channel.routingId, 'dealer'));
    }
    applySocketConfig(dealer, {
      ...client
    });
    return dealer;
  }

  channelRouter(channelName: string): ZLinkBackendRouterSocket {
    const existing = this.channelRouters.get(channelName);
    if (existing !== undefined) {
      return existing;
    }

    const channel = this.registration.channels.get(channelName);
    if (channel?.server === undefined) {
      throw new ZLinkConfigurationException(`Channel server '${channelName}' is not registered.`);
    }
    if (channel.server.bind === undefined) {
      throw new ZLinkConfigurationException(
        `Channel server '${channelName}' does not define a bind endpoint.`
      );
    }

    const router = this.adapter.createRouterSocket(this.context);
    this.channelRouters.set(channelName, router);
    this.registerReceiveFlowSocket(router);
    router.setChannelName(channelName);
    const identity = this.clientServerIdentities.get(channelName) ?? {
      serverRid: channel.server.routingId ?? `${channel.routingIdPrefix ?? 'cs'}-${randomUUID()}`,
      lifecycleGeneration: newLifecycleGeneration()
    };
    this.clientServerIdentities.set(channelName, identity);
    router.setRoutingId(identity.serverRid);
    const publicWeight = channel.server.weight ?? ZLINK_DEFAULT_PUBLIC_WEIGHT;
    this.clientServerPublicWeights.set(channelName, publicWeight);
    router.peerWeight = rawAvailabilityWeight(publicWeight);
    applySocketConfig(router, channel.server);
    router.bind(channel.server.bind);
    if (this.monitoringAdapter !== undefined) {
      const monitor = this.monitoringAdapter.openSocketMonitor(router);
      this.ownedResources.add(monitor);
      monitor.onEvent((event) => {
        if (event.routingId === undefined) return;
        if (
          event.nativeEvent !== ZLinkSocketNativeEventType.Disconnected &&
          event.nativeEvent !== ZLinkSocketNativeEventType.Closed &&
          event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedNoDetail &&
          event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedProtocol &&
          event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedAuth
        )
          return;
        const routingId = String(event.routingId);
        this.clientServerServerPeers.get(channelName)?.delete(routingId);
      });
    }
    const endpoint = advertisedEndpoint(
      router.lastEndpoint ?? channel.server.bind,
      channel.server.advertiseHost
    );
    this.recordListener('clientServer', channelName, endpoint);
    if (!this.clientServerServerDescriptors.has(channelName)) {
      this.clientServerServerDescriptors.set(channelName, {
        channelName,
        serverRid: identity.serverRid,
        lifecycleGeneration: identity.lifecycleGeneration,
        descriptorRevision: 1n,
        endpoint,
        weight: publicWeight,
        state: ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: 'default',
        ownerId: 'manual',
        leaseGeneration: 1n,
        updatedAt: new Date()
      });
    }
    return router;
  }

  clientServerServerIdentity(channelName: string): ZLinkClientServerServerSocketIdentity {
    this.channelRouter(channelName);
    const identity = this.clientServerIdentities.get(channelName);
    const endpoint = this.listenerRecords.endpoint('clientServer', channelName);
    if (identity === undefined || endpoint === undefined) {
      throw new ZLinkConfigurationException(`Channel server '${channelName}' is not registered.`);
    }
    return { ...identity, endpoint };
  }

  fanoutPublisherEndpoint(channelName: string): string | undefined {
    return this.listenerRecords.endpoint('fanout', channelName);
  }

  private recordListener(kind: 'clientServer' | 'fanout', name: string, endpoint: string): void {
    this.listenerRecords.record(kind, name, endpoint);
  }

  clientServerServerSocket(channelName: string): ZLinkBackendRouterSocket {
    return this.channelRouter(channelName);
  }

  clientServerServerWeight(channelName: string): number {
    this.channelRouter(channelName);
    return this.clientServerPublicWeights.get(channelName) ?? ZLINK_DEFAULT_PUBLIC_WEIGHT;
  }

  setClientServerServerWeight(channelName: string, weight: number): void {
    requirePublicWeight(weight);
    const router = this.channelRouter(channelName);
    this.clientServerPublicWeights.set(channelName, weight);
    router.peerWeight = rawAvailabilityWeight(weight);
  }

  openClientServerConnection(
    channelName: string,
    connectionId: string,
    endpoint: string,
    callbacks: ZLinkClientServerConnectionCallbacks
  ): ZLinkBackendDealerSocket {
    if (this.clientServerConnections.has(connectionId)) {
      throw new ZLinkConfigurationException(
        `ClientServer connection '${connectionId}' is already open.`
      );
    }
    if (this.monitoringAdapter === undefined) {
      throw new ZLinkConfigurationException(
        'Automatic ClientServer admission requires socket monitoring.'
      );
    }
    const channel = this.registration.channels.get(channelName);
    const client = channel?.client;
    if (client === undefined) {
      throw new ZLinkConfigurationException(`Channel client '${channelName}' is not registered.`);
    }
    const dealer = this.adapter.createDealerSocket(this.context);
    const connection: ClientServerPhysicalConnection = {
      channelName,
      endpoint,
      dealer,
      aliases: new Set([connectionId]),
      callbacksByAlias: new Map([[connectionId, callbacks]]),
      physicalConnectionId: Symbol(connectionId)
    };
    this.ownedResources.add(dealer);
    const created: (
      | ZLinkBackendSocketMonitor
      | ZLinkBackendReadablePoller
      | ZLinkBackendDealerSocket
      | ZLinkBackendSubscriberSocket
    )[] = [dealer];
    try {
      this.clientServerConnections.set(connectionId, connection);
      this.registerReceiveFlowSocket(dealer);
      dealer.setChannelName(channelName);
      dealer.setRoutingId(`cs-client-${randomUUID()}`);
      applySocketConfig(dealer, {
        ...client
      });
      connection.readablePoller = this.adapter.createReadablePoller(dealer);
      created.push(connection.readablePoller);
      this.ownedResources.add(connection.readablePoller);
      const monitor = this.monitoringAdapter.openSocketMonitor(dealer);
      connection.monitor = monitor;
      created.push(monitor);
      this.ownedResources.add(monitor);
      this.ensureClientServerLivenessTimer();
      monitor.onEvent((event) => {
        if (
          ![...connection.aliases].some(
            (alias) => this.clientServerConnections.get(alias) === connection
          )
        )
          return;
        for (const handler of this.clientServerMonitorHandlers.get(channelName) ?? []) {
          handler(event);
        }
        const routingId = event.routingId === undefined ? undefined : String(event.routingId);
        if (event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady) {
          // This single-endpoint DEALER also receives its disconnected ready-count snapshot.
          if (event.value === 0n) return;
          connection.physicalConnectionId = Symbol(connectionId);
          connection.admissionAttempt = undefined;
          for (const currentCallbacks of [...connection.callbacksByAlias.values()]) {
            currentCallbacks.onTransportReady(routingId ?? '', event.remoteAddr);
          }
          return;
        }
        if (
          event.nativeEvent === ZLinkSocketNativeEventType.Disconnected ||
          event.nativeEvent === ZLinkSocketNativeEventType.Closed ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedNoDetail ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedProtocol ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedAuth
        ) {
          // Core owns the endpoint reconnect (transport liveness §6); the
          // connect intent stays and the next READY starts a new admission.
          this.endClientServerAdmission(connectionId, connection, routingId, event.remoteAddr);
        }
      });
      dealer.connect(endpoint);
      return dealer;
    } catch (error) {
      this.clientServerConnections.delete(connectionId);
      this.unregisterReceiveFlowSocket(dealer);
      void Promise.allSettled(
        created.map(async (resource) => {
          await resource.dispose();
          this.ownedResources.delete(resource);
        })
      ).then((results) => {
        for (const result of results)
          if (result.status === 'rejected') this.oneWayFailureSink?.(result.reason);
      });
      throw error;
    }
  }

  async closeClientServerConnection(connectionId: string): Promise<void> {
    const current = this.clientServerConnections.get(connectionId);
    if (current === undefined) return;
    current.aliases.delete(connectionId);
    current.callbacksByAlias.delete(connectionId);
    const ready = this.clientServerReadyIdentities.get(connectionId);
    this.clientServerReadyIdentities.delete(connectionId);
    if (ready !== undefined && current.readyConnectionId === connectionId) {
      this.clientServerDiscovery.removeClientServer(
        ready.channelName,
        ready.serverRoutingId,
        connectionId
      );
      current.readyConnectionId = undefined;
      const replacement = current.aliases.values().next().value as string | undefined;
      if (
        replacement !== undefined &&
        current.admittedDescriptor !== undefined &&
        this.clientServerDiscovery.admitClientServer(current.admittedDescriptor, replacement)
      ) {
        current.readyConnectionId = replacement;
        this.clientServerReadyIdentities.set(replacement, {
          channelName: current.admittedDescriptor.channelName,
          serverRoutingId: current.admittedDescriptor.serverRoutingId,
          lifecycleGeneration: current.admittedDescriptor.lifecycleGeneration
        });
      }
    }
    this.notifyClientServerTopology(current.channelName);
    if (current.aliases.size > 0) {
      this.clientServerConnections.delete(connectionId);
      return;
    }
    await this.disposeClientServerPhysical(connectionId, current);
    this.clientServerConnections.delete(connectionId);
  }

  private async disposeClientServerPhysical(
    connectionId: string,
    current: ClientServerPhysicalConnection
  ): Promise<void> {
    try {
      current.dealer.disconnect(current.endpoint);
    } catch {
      // The socket close below releases an endpoint that is already disconnected.
    }
    this.unregisterReceiveFlowSocket(current.dealer);
    if (current.readablePoller !== undefined) {
      current.readablePoller.dispose();
      this.ownedResources.delete(current.readablePoller);
      current.readablePoller = undefined;
    }
    const results = await Promise.allSettled([
      current.monitor?.dispose(),
      (async () => {
        await current.dealer.dispose();
        this.ownedResources.delete(current.dealer);
      })()
    ]);
    const errors = results
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `ClientServer connection '${connectionId}' cleanup failed.`);
    }
    if (current.monitor !== undefined) this.ownedResources.delete(current.monitor);
  }

  admitClientServerConnection(
    descriptor: ClientServerDiscoveryDescriptor,
    connectionId: string
  ): boolean {
    const connection = this.clientServerConnections.get(connectionId);
    if (connection === undefined) return false;
    if (connection.readyConnectionId !== undefined) {
      const admitted = this.clientServerDiscovery.admitClientServer(
        descriptor,
        connection.readyConnectionId
      );
      if (admitted) connection.admittedDescriptor = descriptor;
      this.notifyClientServerTopology(descriptor.channelName);
      return admitted;
    }
    const duplicateId = [...this.clientServerReadyIdentities].find(
      ([, identity]) =>
        identity.channelName === descriptor.channelName &&
        identity.serverRoutingId === descriptor.serverRoutingId &&
        identity.lifecycleGeneration === descriptor.lifecycleGeneration
    )?.[0];
    if (duplicateId !== undefined) {
      const shared = this.clientServerConnections.get(duplicateId);
      if (shared !== undefined && shared !== connection) {
        this.clientServerConnections.set(connectionId, shared);
        shared.aliases.add(connectionId);
        const callbacks = connection.callbacksByAlias.get(connectionId);
        if (callbacks !== undefined) shared.callbacksByAlias.set(connectionId, callbacks);
        connection.aliases.clear();
        connection.callbacksByAlias.clear();
        void this.disposeClientServerPhysical(connectionId, connection).catch((error) =>
          this.oneWayFailureSink?.(error)
        );
        return true;
      }
    }
    const admitted = this.clientServerDiscovery.admitClientServer(descriptor, connectionId);
    if (admitted) {
      const now = performance.now();
      connection.liveness = new ServiceLivenessConnection(connectionId, now);
      connection.dealer.receiveAdmission = connection.liveness;
      this.clientServerReadyIdentities.set(connectionId, {
        channelName: descriptor.channelName,
        serverRoutingId: descriptor.serverRoutingId,
        lifecycleGeneration: descriptor.lifecycleGeneration
      });
      connection.readyConnectionId = connectionId;
      connection.admittedDescriptor = descriptor;
    }
    this.notifyClientServerTopology(descriptor.channelName);
    return admitted;
  }

  removeClientServerReady(
    channelName: string,
    serverRoutingId: string,
    connectionId: string
  ): boolean {
    const removed = this.clientServerDiscovery.removeClientServer(
      channelName,
      serverRoutingId,
      connectionId
    );
    if (removed) {
      this.clientServerReadyIdentities.delete(connectionId);
      const connection = this.clientServerConnections.get(connectionId);
      if (connection?.readyConnectionId === connectionId) {
        connection.readyConnectionId = undefined;
        connection.liveness = undefined;
        connection.dealer.receiveAdmission = undefined;
      }
    }
    this.notifyClientServerTopology(channelName);
    return removed;
  }

  selectClientServerDealer(channelName: string): ZLinkBackendDealerSocket | undefined {
    const selected = this.clientServerDiscovery.selectClientServerConnection(channelName);
    if (selected === undefined) return undefined;
    return this.clientServerConnections.get(selected.connectionId)?.dealer;
  }

  clientDealerForOutbound(channelName: string): ZLinkBackendDealerSocket | undefined {
    const channel = this.registration.channels.get(channelName);
    if (channel?.client === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
        `Channel client role '${channelName}' is not configured.`
      );
    }
    return this.selectClientServerDealer(channelName);
  }

  selectedClientServerRid(
    channelName: string,
    dealer: ZLinkBackendDealerSocket
  ): string | undefined {
    for (const connection of this.clientServerConnections.values()) {
      if (connection.channelName !== channelName || connection.dealer !== dealer) continue;
      return connection.admittedDescriptor?.serverRoutingId;
    }
    return undefined;
  }

  /** Observes discovery changes until a server is ready or the runtime closes. */
  async awaitClientDealerForOutbound(
    channelName: string,
    signal?: AbortSignal
  ): Promise<ZLinkBackendDealerSocket | undefined> {
    throwIfAborted(signal);
    const select = (): ZLinkBackendDealerSocket | undefined => {
      const dealer = this.clientDealerForOutbound(channelName);
      if (dealer !== undefined) return dealer;
      if (this.clientServerDiscovery.hasReadyClientServer(channelName)) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `Channel '${channelName}' has no eligible ClientServer member.`
        );
      }
      return undefined;
    };
    const dealer = select();
    if (dealer !== undefined) return dealer;
    return new Promise((resolve, reject) => {
      const cleanup = (): void => {
        unsubscribe();
        signal?.removeEventListener('abort', abort);
      };
      const abort = (): void => {
        cleanup();
        reject(createAbortError());
      };
      const changed = (error?: Error): void => {
        try {
          if (error !== undefined) throw error;
          const selected = select();
          if (selected === undefined) return;
          cleanup();
          resolve(selected);
        } catch (error) {
          cleanup();
          reject(error);
        }
      };
      const unsubscribe = this.clientServerDiscovery.onClientServerChanged(changed);
      signal?.addEventListener('abort', abort, { once: true });
      // Registration and selection share one event-loop turn; recheck after subscribing.
      changed();
      if (signal?.aborted === true) abort();
    });
  }

  startManualClientServerConnections(): void {
    for (const [channelName, channel] of this.registration.channels) {
      const client = channel.client;
      if (client === undefined) continue;
      const endpoints = channel.client?.manualConnections ?? [];
      if (endpoints.length === 0) continue;
      const open = (endpoint: string) =>
        this.openManualClientServerConnection(channelName, endpoint);
      const close = (endpoint: string) => {
        void this.closeClientServerConnection(
          manualClientServerConnectionId(channelName, endpoint)
        ).catch((error) => this.oneWayFailureSink?.(error));
      };
      attachEndpointConnections(client, { connect: open, disconnect: close });
      for (const endpoint of endpoints) {
        open(endpoint);
      }
    }
  }

  startLocalClientServerConnections(): void {
    for (const [channelName, channel] of this.registration.channels) {
      if (channel.client === undefined || channel.server === undefined) continue;
      const identity = this.clientServerServerIdentity(channelName);
      this.openConfiguredClientServerConnection(
        channelName,
        localClientServerConnectionId(channelName),
        identity.endpoint
      );
    }
  }

  clientServerMonitoringSource(channelName: string): ZLinkBackendSocketMonitor & {
    onChange(handler: () => void): void;
  } {
    const channel = this.registration.channels.get(channelName);
    if (channel?.client === undefined && channel?.server === undefined) {
      throw new ZLinkConfigurationException(
        `ClientServer channel '${channelName}' is not registered.`
      );
    }
    let disposed = false;
    let handler: ((event?: ZLinkBackendSocketMonitorEvent) => void) | undefined;
    const register = (next: (event?: ZLinkBackendSocketMonitorEvent) => void): void => {
      if (disposed) return;
      if (handler !== undefined) {
        this.clientServerMonitorHandlers.get(channelName)?.delete(handler);
      }
      handler = next;
      let handlers = this.clientServerMonitorHandlers.get(channelName);
      if (handlers === undefined) {
        handlers = new Set();
        this.clientServerMonitorHandlers.set(channelName, handlers);
      }
      handlers.add(next);
    };
    return {
      nativeInstance: {},
      onEvent: (next) => {
        register((event) => {
          if (event !== undefined) next(event);
        });
      },
      onChange: (next) => {
        register((event) => {
          if (event === undefined) next();
        });
      },
      drain: () => 0,
      dispose: async () => {
        disposed = true;
        if (handler !== undefined) {
          const handlers = this.clientServerMonitorHandlers.get(channelName);
          handlers?.delete(handler);
          if (handlers?.size === 0) this.clientServerMonitorHandlers.delete(channelName);
        }
      }
    };
  }

  clientServerActiveTargets(
    channelName: string
  ): readonly Pick<ClientServerDiscoveryDescriptor, 'serverRoutingId' | 'weight' | 'state'>[] {
    const descriptors = this.clientServerDiscovery.clientServerDescriptors(channelName);
    const local = this.clientServerServerDescriptors.get(channelName);
    if (
      local === undefined ||
      descriptors.some(
        (descriptor) =>
          descriptor.serverRoutingId === local.serverRid &&
          descriptor.lifecycleGeneration === local.lifecycleGeneration
      )
    )
      return descriptors;

    return [
      ...descriptors,
      {
        serverRoutingId: local.serverRid,
        weight: local.weight,
        state: discoveryAvailabilityForRuntimeState(local.state)
      }
    ].sort((left, right) =>
      left.serverRoutingId < right.serverRoutingId
        ? -1
        : left.serverRoutingId > right.serverRoutingId
          ? 1
          : 0
    );
  }

  fanoutActiveTargets(channelName: string) {
    return this.clientServerDiscovery.fanoutEndpoints(channelName);
  }

  admitFanoutPublisher(descriptor: ZLinkFanoutPublisherDescriptor, connectionId: string): boolean {
    const connection = this.fanoutConnections.get(connectionId);
    if (connection === undefined || !connection.ready) return false;
    const admitted = this.clientServerDiscovery.admitFanoutPublisher(
      {
        channelName: descriptor.channelName,
        publisherRoutingId: String(descriptor.publisherRid),
        lifecycleGeneration: descriptor.lifecycleGeneration,
        descriptorRevision: descriptor.descriptorRevision,
        advertisedEndpoint: descriptor.endpoint,
        state: discoveryAvailabilityForRuntimeState(descriptor.state)
      },
      fanoutDiscoveryConnectionId(connectionId)
    );
    this.notifyFanoutTopology(descriptor.channelName);
    return admitted;
  }

  removeFanoutPublisher(descriptor: ZLinkFanoutPublisherDescriptor, connectionId: string): boolean {
    const removed = this.clientServerDiscovery.removeFanoutPublisher(
      descriptor.channelName,
      String(descriptor.publisherRid),
      fanoutDiscoveryConnectionId(connectionId)
    );
    this.notifyFanoutTopology(descriptor.channelName);
    return removed;
  }

  setClientServerServerDescriptor(
    descriptor: ZLinkClientServerServerDescriptor | undefined,
    channelName: string
  ): void {
    if (descriptor === undefined) {
      this.clientServerServerDescriptors.delete(channelName);
    } else {
      this.clientServerServerDescriptors.set(channelName, descriptor);
      this.pushClientServerDescriptorUpdate(channelName, descriptor);
    }
    this.notifyClientServerTopology(channelName);
  }

  private notifyClientServerTopology(channelName: string): void {
    for (const handler of this.clientServerMonitorHandlers.get(channelName) ?? []) handler();
  }

  clientServerServerPeersForChannel(channelName: string): Map<RoutingId, ClientServerServerPeer> {
    let peers = this.clientServerServerPeers.get(channelName);
    if (peers === undefined) {
      peers = new Map();
      this.clientServerServerPeers.set(channelName, peers);
    }
    return peers;
  }

  tryHandleClientServerControl(
    channelName: string,
    received: {
      readonly parts: readonly Message[];
      readonly replyToken: unknown | null;
      readonly routingId: unknown;
    },
    router: ZLinkBackendRouterSocket,
    peers = this.clientServerServerPeersForChannel(channelName)
  ): boolean {
    const peer = peers.get(received.routingId as RoutingId);
    peer?.liveness.recordReceived();
    if (received.parts.length === 0) return false;
    const first = received.parts[0];
    if (!isClientServerControlFrame(first.data())) return false;
    let reply: Buffer;
    try {
      const record = decodeClientServerControl(first.data());
      if (
        record.kind === 'livenessProbe' &&
        received.parts.length === 1 &&
        received.replyToken !== null
      ) {
        reply = encodeClientServerLivenessAck(record.probeId);
      } else if (
        record.kind === 'livenessAck' &&
        received.parts.length === 1 &&
        received.replyToken === null
      ) {
        if (!this.acceptClientServerServerLivenessAck(peer, record.probeId)) {
          this.reportStaleClientServerLivenessAck(
            channelName,
            String(received.routingId),
            record.probeId
          );
        }
        return true;
      } else if (
        record.kind !== 'hello' ||
        received.parts.length !== 1 ||
        received.replyToken === null
      ) {
        reply = encodeClientServerReject(ClientServerRejectReason.ProtocolVersionUnsupported);
      } else {
        const descriptor = this.clientServerServerDescriptors.get(channelName);
        if (
          descriptor === undefined ||
          record.hello.channelName !== channelName ||
          record.hello.securityIdentity !== descriptor.securityIdentity
        ) {
          reply = encodeClientServerReject(ClientServerRejectReason.AdmissionMismatch);
        } else {
          const normalizedEffectiveMaxMessageBytes = Math.min(
            normalizeClientServerMessageLimit(router.maxMessageSize),
            record.hello.normalizedEffectiveMaxMessageBytes
          );
          reply = encodeClientServerAdmit(descriptor, normalizedEffectiveMaxMessageBytes);
          this.admitClientServerServerPeer(
            channelName,
            String(received.routingId),
            normalizedEffectiveMaxMessageBytes
          );
        }
      }
    } catch {
      reply = encodeClientServerReject(1);
    }
    if (received.replyToken !== null) {
      const message = RuntimeMessage.from(reply);
      try {
        router.reply(received.routingId as RoutingId, received.replyToken, message);
      } finally {
        message.close();
      }
    }
    return true;
  }

  tickClientServerLiveness(nowMs = performance.now()): void {
    this.drainSocketMonitors();
    for (const connection of new Set(this.clientServerConnections.values())) {
      const connectionId =
        connection.readyConnectionId ??
        (connection.aliases.values().next().value as string | undefined);
      if (connectionId === undefined) continue;
      this.drainClientServerControl(connectionId, connection);
      if (connection.liveness === undefined) continue;
      if (connection.liveness.isExpired(nowMs)) {
        this.restartClientServerAdmission(connectionId, connection);
        continue;
      }
      const probeId = connection.liveness.tryGetProbe(nowMs, () =>
        this.allocateClientServerProbeId()
      );
      if (probeId === undefined) continue;
      this.requestClientServerLiveness(connectionId, connection, probeId);
    }

    for (const peers of this.clientServerServerPeers.values()) {
      for (const [routingId, peer] of peers) {
        if (peer.liveness.isExpired(nowMs)) {
          peers.delete(routingId);
          try {
            this.channelRouters.get(peer.channelName)?.disconnectPeer(peer.routingId);
          } catch (error) {
            this.oneWayFailureSink?.(error);
          }
          continue;
        }
        const probeId = peer.liveness.tryGetProbe(nowMs, () => this.allocateClientServerProbeId());
        if (probeId !== undefined) this.requestClientServerServerLiveness(peer, probeId);
      }
    }
    this.tickFanoutLiveness(nowMs);
  }

  openFanoutSubscriberConnection(
    channelName: string,
    connectionId: string,
    endpoint: string,
    callbacks: ZLinkFanoutConnectionCallbacks
  ): ZLinkBackendSubscriberSocket {
    if (this.fanoutConnections.has(connectionId)) {
      throw new ZLinkConfigurationException(`Fanout connection '${connectionId}' is already open.`);
    }
    if (this.monitoringAdapter === undefined) {
      throw new ZLinkConfigurationException(
        'Fanout subscriber connections require socket monitoring.'
      );
    }
    const channel = this.registration.channels.get(channelName);
    if (channel?.subscriber === undefined) {
      throw new ZLinkConfigurationException(
        `Fanout subscriber '${channelName}' is not registered.`
      );
    }
    const subscriber = this.adapter.createSubscriberSocket(this.context);
    const connection: FanoutPublisherConnection = {
      channelName,
      endpoint,
      subscriber,
      callbacks,
      physicalConnectionId: Symbol(connectionId),
      transportReady: false,
      ready: false
    };
    this.ownedResources.add(subscriber);
    const created: (
      | ZLinkBackendSocketMonitor
      | ZLinkBackendReadablePoller
      | ZLinkBackendDealerSocket
      | ZLinkBackendSubscriberSocket
    )[] = [subscriber];
    try {
      this.fanoutConnections.set(connectionId, connection);
      subscriber.setChannelName(channelName);
      setFanoutSubscriptions(subscriber, channel.subscriptions);
      const monitor = this.monitoringAdapter.openSocketMonitor(subscriber);
      connection.monitor = monitor;
      created.push(monitor);
      this.ownedResources.add(monitor);
      this.ensureClientServerLivenessTimer();
      monitor.onEvent((event) => {
        if (this.fanoutConnections.get(connectionId) !== connection) return;
        for (const handler of this.fanoutMonitorHandlers.get(channelName) ?? []) {
          handler(event);
        }
        if (event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady) {
          connection.transportReady = true;
          return;
        }
        if (
          event.nativeEvent === ZLinkSocketNativeEventType.Disconnected ||
          event.nativeEvent === ZLinkSocketNativeEventType.Closed ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedNoDetail ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedProtocol ||
          event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedAuth
        ) {
          connection.transportReady = false;
          connection.ready = false;
          connection.deadlineAt = undefined;
          callbacks.onTerminated('disconnect');
        }
      });
      subscriber.connect(endpoint);
      return subscriber;
    } catch (error) {
      this.fanoutConnections.delete(connectionId);
      void Promise.allSettled(
        created.map(async (resource) => {
          await resource.dispose();
          this.ownedResources.delete(resource);
        })
      ).then((results) => {
        for (const result of results)
          if (result.status === 'rejected') this.oneWayFailureSink?.(result.reason);
      });
      throw error;
    }
  }

  async closeFanoutSubscriberConnection(connectionId: string): Promise<void> {
    const connection = this.fanoutConnections.get(connectionId);
    if (connection === undefined) return;
    try {
      connection.subscriber.disconnect(connection.endpoint);
    } catch {
      // The socket close below releases an endpoint that is already disconnected.
    }
    const results = await Promise.allSettled([
      connection.monitor?.dispose(),
      (async () => {
        await connection.subscriber.dispose();
        this.ownedResources.delete(connection.subscriber);
      })()
    ]);
    const errors = results
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `Fanout connection '${connectionId}' cleanup failed.`);
    }
    this.fanoutConnections.delete(connectionId);
    if (connection.monitor !== undefined) this.ownedResources.delete(connection.monitor);
  }

  handleFanoutInbound(
    connectionId: string,
    topicMessage: {
      readonly topic: string;
      readonly parts: readonly { data(): Uint8Array }[];
    },
    source: ZLinkBackendSubscriberSocket,
    nowMs = performance.now()
  ): boolean {
    return this.handleFanoutInboundResult(connectionId, topicMessage, source, nowMs).consumed;
  }

  handleFanoutInboundResult(
    connectionId: string,
    topicMessage: {
      readonly topic: string;
      readonly parts: readonly { data(): Uint8Array }[];
    },
    source: ZLinkBackendSubscriberSocket,
    nowMs = performance.now()
  ): {
    readonly consumed: boolean;
    readonly decodedHeader?: ZLinkChannelEnvelopeHeader;
  } {
    const connection = this.fanoutConnections.get(connectionId);
    if (connection === undefined || connection.subscriber !== source) {
      return { consumed: true };
    }
    const classification = inspectFanoutInbound(topicMessage.topic, topicMessage.parts);
    if (classification.kind === 'protocolError') {
      connection.ready = false;
      connection.deadlineAt = undefined;
      connection.callbacks.onTerminated('protocol');
      return { consumed: true };
    }
    connection.deadlineAt = nowMs + CLIENT_SERVER_PEER_DEADLINE_MS;
    if (!connection.ready) {
      connection.ready = true;
      connection.callbacks.onReady();
    }
    return classification.kind === 'beacon'
      ? { consumed: true }
      : { consumed: false, decodedHeader: classification.header };
  }

  isFanoutConnectionReady(connectionId: string): boolean {
    return this.fanoutConnections.get(connectionId)?.ready === true;
  }

  fanoutMonitoringSource(channelName: string): ZLinkBackendSocketMonitor {
    if (this.registration.channels.get(channelName)?.subscriber === undefined) {
      throw new ZLinkConfigurationException(
        `Fanout subscriber '${channelName}' is not registered.`
      );
    }
    let handler: ((event: ZLinkBackendSocketMonitorEvent) => void) | undefined;
    let disposed = false;
    return {
      nativeInstance: {},
      onEvent: (next) => {
        if (disposed) return;
        if (handler !== undefined) {
          this.fanoutMonitorHandlers.get(channelName)?.delete(handler);
        }
        handler = next;
        let handlers = this.fanoutMonitorHandlers.get(channelName);
        if (handlers === undefined) {
          handlers = new Set();
          this.fanoutMonitorHandlers.set(channelName, handlers);
        }
        handlers.add(next);
      },
      drain: () => 0,
      dispose: async () => {
        disposed = true;
        if (handler !== undefined) {
          this.fanoutMonitorHandlers.get(channelName)?.delete(handler);
        }
      }
    };
  }

  fanoutTopologyMonitoringSource(channelName: string): {
    onChange(handler: () => void): void;
    dispose(): Promise<void>;
  } {
    if (this.registration.channels.get(channelName)?.subscriber === undefined) {
      throw new ZLinkConfigurationException(
        `Fanout subscriber '${channelName}' is not registered.`
      );
    }
    let handler: (() => void) | undefined;
    let disposed = false;
    return {
      onChange: (next) => {
        if (disposed) return;
        if (handler !== undefined) {
          this.fanoutTopologyHandlers.get(channelName)?.delete(handler);
        }
        handler = next;
        let handlers = this.fanoutTopologyHandlers.get(channelName);
        if (handlers === undefined) {
          handlers = new Set();
          this.fanoutTopologyHandlers.set(channelName, handlers);
        }
        handlers.add(next);
      },
      dispose: async () => {
        disposed = true;
        if (handler !== undefined) {
          const handlers = this.fanoutTopologyHandlers.get(channelName);
          handlers?.delete(handler);
          if (handlers?.size === 0) this.fanoutTopologyHandlers.delete(channelName);
        }
      }
    };
  }

  notifyFanoutTopology(channelName: string): void {
    for (const handler of this.fanoutTopologyHandlers.get(channelName) ?? []) {
      try {
        handler();
      } catch {
        // Monitoring observers cannot change transport lifecycle results.
      }
    }
  }

  private tickFanoutLiveness(nowMs: number): void {
    for (const [channelName, publisher] of this.publishers) {
      const next = this.fanoutPublisherNextBeacon.get(channelName) ?? nowMs;
      if (nowMs < next) continue;
      this.fanoutPublisherNextBeacon.set(channelName, nowMs + CLIENT_SERVER_PROBE_INTERVAL_MS);
      const payload = RuntimeMessage.from(FANOUT_LIVENESS_PAYLOAD);
      try {
        publisher.publish(FANOUT_LIVENESS_TOPIC, payload);
      } catch (error) {
        this.oneWayFailureSink?.(error);
      } finally {
        payload.close();
      }
    }
    for (const [connectionId, connection] of [...this.fanoutConnections]) {
      if (connection.deadlineAt === undefined || nowMs < connection.deadlineAt) continue;
      connection.ready = false;
      connection.deadlineAt = undefined;
      connection.callbacks.onTerminated('deadline');
      if (this.fanoutConnections.get(connectionId) === connection) {
        void this.closeFanoutSubscriberConnection(connectionId).catch((error) =>
          this.oneWayFailureSink?.(error)
        );
      }
    }
  }

  private openManualClientServerConnection(channelName: string, endpoint: string): void {
    this.openConfiguredClientServerConnection(
      channelName,
      manualClientServerConnectionId(channelName, endpoint),
      endpoint
    );
  }

  private openConfiguredClientServerConnection(
    channelName: string,
    connectionId: string,
    endpoint: string
  ): void {
    if (this.clientServerConnections.has(connectionId)) return;
    this.openClientServerConnection(channelName, connectionId, endpoint, {
      onTransportReady: () => {
        void this.admitConfiguredClientServerConnection(channelName, connectionId).catch((error) =>
          this.oneWayFailureSink?.(error)
        );
      },
      onTerminated: () => this.removeReadyConnection(connectionId)
    });
  }

  private async admitConfiguredClientServerConnection(
    channelName: string,
    connectionId: string
  ): Promise<void> {
    const connection = this.clientServerConnections.get(connectionId);
    if (connection === undefined) return;
    const physicalConnectionId = connection.physicalConnectionId;
    if (connection.admissionAttempt !== undefined) return;
    const admissionAttempt = Symbol(connectionId);
    connection.admissionAttempt = admissionAttempt;
    let retryAdmission = false;
    try {
      const admission = await requestClientServerAdmission(
        connection.dealer,
        channelName,
        'default',
        15_000
      );
      if (
        this.clientServerConnections.get(connectionId) !== connection ||
        connection.physicalConnectionId !== physicalConnectionId ||
        connection.admissionAttempt !== admissionAttempt
      )
        return;
      if (admission.channelName !== channelName || admission.securityIdentity !== 'default') {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission does not match its configured identity.`
        );
      }
      if (
        !this.admitClientServerConnection(admissionToDiscoveryDescriptor(admission), connectionId)
      ) {
        throw new ZLinkConfigurationException(`ClientServer '${channelName}' admission was stale.`);
      }
    } catch (error) {
      if (
        this.clientServerConnections.get(connectionId) !== connection ||
        connection.physicalConnectionId !== physicalConnectionId ||
        connection.admissionAttempt !== admissionAttempt
      )
        return;
      this.removeReadyConnection(connectionId);
      if (isBackendRequestTimeoutError(error)) {
        retryAdmission = true;
        return;
      }
      throw error;
    } finally {
      if (
        this.clientServerConnections.get(connectionId) === connection &&
        connection.physicalConnectionId === physicalConnectionId &&
        connection.admissionAttempt === admissionAttempt
      ) {
        connection.admissionAttempt = undefined;
        if (retryAdmission) {
          void this.admitConfiguredClientServerConnection(channelName, connectionId).catch(
            (error) => this.oneWayFailureSink?.(error)
          );
        }
      }
    }
  }

  private drainClientServerControl(
    connectionId: string,
    connection: ClientServerPhysicalConnection
  ): void {
    const admission = connection.liveness;
    for (;;) {
      if (connection.readablePoller === undefined || !connection.readablePoller.wait(0)) return;
      const received = connection.dealer.recv(1);
      if (received === undefined) {
        connection.readablePoller.markDrained();
        return;
      }
      admission?.recordReceived();
      try {
        if (received.parts.length !== 1 || !isClientServerControlFrame(received.parts[0]!.data())) {
          throw new ZLinkConfigurationException(
            `ClientServer '${connection.channelName}' received an unsolicited application frame.`
          );
        }
        const record = decodeClientServerControl(received.parts[0]!.data());
        if (record.kind === 'livenessProbe') {
          const ack = RuntimeMessage.from(encodeClientServerLivenessAck(record.probeId));
          void connection.dealer
            .send(ack)
            .catch((error) => {
              this.removeReadyConnection(connectionId);
              this.oneWayFailureSink?.(error);
            })
            .finally(() => ack.close());
          continue;
        }
        if (record.kind !== 'update') {
          throw new ZLinkConfigurationException(
            `ClientServer '${connection.channelName}' received an invalid pushed control record.`
          );
        }
        this.applyClientServerDescriptorUpdate(connectionId, connection, record.admission);
      } catch (error) {
        this.restartClientServerAdmission(connectionId, connection);
        this.oneWayFailureSink?.(error);
        return;
      } finally {
        received.close();
      }
    }
  }

  /**
   * Ends the current logical admission of this connection: later replies of
   * the fenced attempt no longer apply and its ready target is withdrawn.
   */
  private endClientServerAdmission(
    connectionId: string,
    connection: ClientServerPhysicalConnection,
    routingId: string | undefined,
    endpoint: string
  ): void {
    connection.physicalConnectionId = Symbol(connectionId);
    connection.admissionAttempt = undefined;
    if (connection.readyConnectionId !== undefined) {
      this.removeReadyConnection(connection.readyConnectionId);
    }
    for (const callbacks of [...connection.callbacksByAlias.values()]) {
      callbacks.onTerminated(routingId, endpoint);
    }
  }

  /**
   * A peer deadline or an invalid pushed control ends only the current logical
   * admission. The connect intent stays with Core, which owns the endpoint
   * reconnect (transport liveness §6); the service handshake starts again on
   * the existing admission path.
   */
  private restartClientServerAdmission(
    connectionId: string,
    connection: ClientServerPhysicalConnection
  ): void {
    this.endClientServerAdmission(connectionId, connection, undefined, connection.endpoint);
    for (const callbacks of [...connection.callbacksByAlias.values()]) {
      callbacks.onTransportReady('', connection.endpoint);
    }
  }

  private applyClientServerDescriptorUpdate(
    connectionId: string,
    connection: { readonly channelName: string; readonly physicalConnectionId: symbol },
    admission: ZLinkClientServerAdmission
  ): void {
    const currentConnection = this.clientServerConnections.get(connectionId);
    if (
      currentConnection === undefined ||
      currentConnection.physicalConnectionId !== connection.physicalConnectionId
    )
      return;
    const identity = this.clientServerReadyIdentities.get(connectionId);
    if (identity === undefined) return;
    const current = this.clientServerDiscovery
      .clientServerDescriptors(connection.channelName)
      .find((value) => value.serverRoutingId === identity.serverRoutingId);
    if (current === undefined) return;
    if (
      admission.channelName !== current.channelName ||
      admission.serverRid !== current.serverRoutingId ||
      admission.lifecycleGeneration !== current.lifecycleGeneration ||
      admission.securityIdentity !== current.securityIdentity ||
      admission.advertisedEndpoint !== current.advertisedEndpoint
    ) {
      throw new ZLinkConfigurationException(
        `ClientServer '${connection.channelName}' descriptor update changed immutable identity.`
      );
    }
    const candidate = admissionToDiscoveryDescriptor(admission);
    if (candidate.effectiveMaxMessageBytes !== current.effectiveMaxMessageBytes) {
      throw new ServiceWireProtocolError(
        `ClientServer '${connection.channelName}' descriptor update changed the admitted message bound.`
      );
    }
    if (candidate.descriptorRevision < current.descriptorRevision) {
      throw new ServiceWireProtocolError(
        `ClientServer '${connection.channelName}' descriptor revision is stale.`
      );
    }
    if (candidate.descriptorRevision === current.descriptorRevision) {
      if (!sameClientServerDiscoveryDescriptor(candidate, current)) {
        throw new ServiceWireProtocolError(
          `ClientServer '${connection.channelName}' descriptor revision conflicts.`
        );
      }
      return;
    }
    if (!this.admitClientServerConnection(candidate, connectionId)) {
      throw new ZLinkConfigurationException(
        `ClientServer '${connection.channelName}' descriptor update was fenced.`
      );
    }
  }

  private ensureClientServerLivenessTimer(): void {
    if (this.clientServerLivenessTimer !== undefined) return;
    this.clientServerLivenessTimer = setInterval(
      () => this.tickClientServerLiveness(),
      CLIENT_SERVER_LIVENESS_TICK_MS
    );
    this.clientServerLivenessTimer.unref();
  }

  private drainSocketMonitors(): void {
    for (const monitor of this.ownedResources) if ('drain' in monitor) monitor.drain();
  }

  private removeReadyConnection(connectionId: string): void {
    const identity = this.clientServerReadyIdentities.get(connectionId);
    if (identity === undefined) return;
    this.clientServerDiscovery.markClientServerDisconnected(
      identity.channelName,
      identity.serverRoutingId,
      connectionId
    );
    this.clientServerReadyIdentities.delete(connectionId);
    const connection = this.clientServerConnections.get(connectionId);
    if (connection !== undefined) {
      if (connection.readyConnectionId === connectionId) {
        connection.readyConnectionId = undefined;
      }
      connection.liveness = undefined;
      connection.dealer.receiveAdmission = undefined;
    }
    this.notifyClientServerTopology(identity.channelName);
  }

  private requestClientServerLiveness(
    connectionId: string,
    connection: ClientServerPhysicalConnection,
    probeId: bigint
  ): void {
    const admission = connection.liveness;
    const message = RuntimeMessage.from(encodeClientServerLivenessProbe(probeId));
    void connection.dealer
      .request(message, CLIENT_SERVER_PEER_DEADLINE_MS)
      .then((parts) => {
        try {
          const current = this.clientServerConnections.get(connectionId);
          if (
            current === undefined ||
            current.physicalConnectionId !== connection.physicalConnectionId ||
            current.liveness !== admission ||
            parts.length !== 1
          )
            return;
          const record = decodeClientServerControl(parts[0]!.data());
          if (record.kind !== 'livenessAck') return;
          if (
            admission === undefined ||
            !admission.acknowledge(record.probeId, performance.now())
          ) {
            this.reportStaleClientServerLivenessAck(
              connection.channelName,
              connectionId,
              record.probeId
            );
            return;
          }
        } finally {
          closeMessages(parts);
        }
      })
      .catch((error) => this.oneWayFailureSink?.(error))
      .finally(() => message.close());
  }

  private admitClientServerServerPeer(
    channelName: string,
    routingId: RoutingId,
    normalizedEffectiveMaxMessageBytes: number
  ): void {
    const peer: ClientServerServerPeer = {
      channelName,
      routingId,
      normalizedEffectiveMaxMessageBytes,
      liveness: new ServiceLivenessConnection(
        clientServerServerPeerKey(channelName, routingId),
        performance.now()
      )
    };
    this.clientServerServerPeersForChannel(channelName).set(routingId, peer);
    this.ensureClientServerLivenessTimer();
  }

  private requestClientServerServerLiveness(peer: ClientServerServerPeer, probeId: bigint): void {
    const router = this.channelRouters.get(peer.channelName);
    if (router === undefined) return;
    const message = RuntimeMessage.from(encodeClientServerLivenessProbe(probeId));
    void router
      .send(peer.routingId, message)
      .catch((error) => this.oneWayFailureSink?.(error))
      .finally(() => message.close());
  }

  private acceptClientServerServerLivenessAck(
    peer: ClientServerServerPeer | undefined,
    probeId: bigint
  ): boolean {
    return peer?.liveness.acknowledge(probeId, performance.now()) ?? false;
  }

  private reportStaleClientServerLivenessAck(
    channelName: string,
    peer: string,
    probeId: bigint
  ): void {
    this.oneWayFailureSink?.(
      createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestProtocolError,
        `ClientServer '${channelName}' ignored stale or duplicate liveness ACK '${probeId}' from '${peer}'.`,
        false
      )
    );
  }

  private pushClientServerDescriptorUpdate(
    channelName: string,
    descriptor: ZLinkClientServerServerDescriptor
  ): void {
    const router = this.channelRouters.get(channelName);
    const peers = this.clientServerServerPeers.get(channelName);
    if (router === undefined || peers === undefined) return;
    for (const [routingId, peer] of peers) {
      const message = RuntimeMessage.from(
        encodeClientServerUpdate(descriptor, peer.normalizedEffectiveMaxMessageBytes)
      );
      void router
        .send(routingId, message)
        .catch((error) => {
          this.oneWayFailureSink?.(error);
        })
        .finally(() => message.close());
    }
  }

  private allocateClientServerProbeId(): bigint {
    const result = this.nextClientServerProbeId;
    this.nextClientServerProbeId = result === UINT64_MAX ? 1n : result + 1n;
    return result;
  }

  publisher(channelName: string): ZLinkBackendPublisherSocket {
    const existing = this.publishers.get(channelName);
    if (existing !== undefined) {
      return existing;
    }

    const channel = this.registration.channels.get(channelName);
    if (channel?.publisher === undefined) {
      throw new ZLinkConfigurationException(
        `Channel publisher '${channelName}' is not registered.`
      );
    }
    if (channel.publisher.bind === undefined) {
      throw new ZLinkConfigurationException(
        `Channel publisher '${channelName}' does not define a bind endpoint.`
      );
    }

    const publisher = this.adapter.createPublisherSocket(this.context);
    this.publishers.set(channelName, publisher);
    publisher.setChannelName(channelName);
    applyFanoutPublisherSocketOptions(publisher, channel);
    publisher.bind(channel.publisher.bind);
    const endpoint = buildAdvertisedEndpoint(
      publisher.lastEndpoint ?? channel.publisher.bind,
      channel.publisher.advertiseHost
    );
    if (endpoint !== undefined && endpoint.length > 0) {
      this.recordListener('fanout', channelName, endpoint);
    }
    this.fanoutPublisherNextBeacon.set(
      channelName,
      performance.now() + CLIENT_SERVER_PROBE_INTERVAL_MS
    );
    this.ensureClientServerLivenessTimer();
    return publisher;
  }

  routeRouter(routerChannelId: string): ZLinkBackendRouterSocket {
    const existing = this.routeRouters.get(routerChannelId);
    if (existing !== undefined) {
      return existing;
    }

    const routeChannel = this.registration.routeChannelOptions.get(routerChannelId);
    if (routeChannel === undefined) {
      throw new ZLinkConfigurationException(
        `Route channel '${routerChannelId}' is not registered.`
      );
    }
    const router = this.adapter.createRouterSocket(this.context);
    this.routeRouters.set(routerChannelId, router);
    this.registerReceiveFlowSocket(router);
    router.setChannelName(routerChannelId);
    if (
      (routeChannel.manualConnections?.length ?? 0) > 0 &&
      typeof router.setProbe === 'function'
    ) {
      router.setProbe(true);
    }
    router.setRoutingId(
      routeChannel.routingId ?? `${routeChannel.routingIdPrefix ?? routerChannelId}-${randomUUID()}`
    );
    const publicWeight = routeChannel.weight ?? ZLINK_DEFAULT_PUBLIC_WEIGHT;
    this.routeMeshPublicWeights.set(routerChannelId, publicWeight);
    router.peerWeight = rawAvailabilityWeight(publicWeight);
    applySocketConfig(router, routeChannel);
    this.trackRouteMonitor(routerChannelId, router);
    if ((routeChannel.manualConnections ?? []).length > 0) {
      for (const endpoint of routeChannel.manualConnections ?? []) {
        router.connect(endpoint);
      }
    }
    attachEndpointConnections(routeChannel, router);
    if (routeChannel.bind !== undefined && routeChannel.bind.trim().length > 0) {
      router.bind(routeChannel.bind);
    }
    return router;
  }

  routeMeshSocket(routerChannelId: string): ZLinkBackendRouterSocket {
    return this.routeRouter(routerChannelId);
  }

  routeMeshWeight(routerChannelId: string): number {
    this.routeRouter(routerChannelId);
    return this.routeMeshPublicWeights.get(routerChannelId) ?? ZLINK_DEFAULT_PUBLIC_WEIGHT;
  }

  setRouteMeshWeight(routerChannelId: string, weight: number): void {
    requirePublicWeight(weight);
    const router = this.routeRouter(routerChannelId);
    this.routeMeshPublicWeights.set(routerChannelId, weight);
    router.peerWeight = rawAvailabilityWeight(weight);
  }

  private registerReceiveFlowSocket(socket: ReceiveFlowSocket): void {
    this.applicationJobQueue?.registerReceiveFlowTarget(
      socket,
      (state) => socket.setReceiveFlowState(state === 'paused' ? 1 : 0),
      this.oneWayFailureSink
    );
  }

  private unregisterReceiveFlowSocket(socket: ReceiveFlowSocket): void {
    this.applicationJobQueue?.unregisterReceiveFlowTarget(socket);
  }

  routeMemberStatus(
    routerChannelId: string,
    targetNodeRid: string
  ): 'unknown' | 'missing' | 'connected' | 'disconnected' {
    return this.routeMembers.status(routerChannelId, targetNodeRid);
  }

  monitorDisconnects(
    socket: ZLinkBackendDealerSocket | ZLinkBackendSubscriberSocket,
    handler: (endpoint: string) => void
  ): void {
    if (this.monitoringAdapter === undefined) {
      return;
    }
    const monitor = this.monitoringAdapter.openSocketMonitor(socket);
    this.ownedResources.add(monitor);
    monitor.onEvent((event) => {
      if (
        event.nativeEvent === ZLinkSocketNativeEventType.Disconnected &&
        event.remoteAddr.length > 0
      ) {
        handler(event.remoteAddr);
      }
    });
  }

  private trackRouteMonitor(routerChannelId: string, router: ZLinkBackendRouterSocket): void {
    if (this.monitoringAdapter === undefined) {
      return;
    }
    const monitor = this.monitoringAdapter.openSocketMonitor(router);
    this.ownedResources.add(monitor);
    monitor.onEvent((event) => {
      const routingId = event.routingId === undefined ? undefined : String(event.routingId);
      if (
        event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady &&
        routingId !== undefined
      ) {
        this.routeMembers.observeReady(routerChannelId, routingId, event.remoteAddr);
        return;
      }
      if (
        event.nativeEvent !== ZLinkSocketNativeEventType.Disconnected &&
        event.nativeEvent !== ZLinkSocketNativeEventType.Closed
      )
        return;
      this.routeMembers.observeTermination(routerChannelId, routingId, event.remoteAddr);
    });
  }
}

function manualClientServerConnectionId(channelName: string, endpoint: string): string {
  return `manual:${channelName.length}:${channelName}:${endpoint.length}:${endpoint}`;
}

function localClientServerConnectionId(channelName: string): string {
  return `local:${channelName.length}:${channelName}`;
}

function clientServerServerPeerKey(channelName: string, routingId: RoutingId): string {
  return `${channelName.length}:${channelName}:${String(routingId)}`;
}

async function requestClientServerAdmission(
  dealer: ZLinkBackendDealerSocket,
  channelName: string,
  securityIdentity: string,
  timeoutMs: number
): Promise<ZLinkClientServerAdmission> {
  const message = RuntimeMessage.from(
    encodeClientServerHello({
      channelName,
      securityIdentity,
      normalizedEffectiveMaxMessageBytes: normalizeClientServerMessageLimit(dealer.maxMessageSize)
    })
  );
  try {
    const parts = await dealer.request(message, timeoutMs);
    try {
      if (parts.length !== 1) {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission request returned an invalid reply.`
        );
      }
      const record = decodeClientServerControl(parts[0]!.data());
      if (record.kind === 'reject') {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission was rejected (${record.reason}).`
        );
      }
      if (record.kind !== 'admit') {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission reply has an invalid command.`
        );
      }
      return record.admission;
    } finally {
      closeMessages(parts);
    }
  } finally {
    message.close();
  }
}

function admissionToDiscoveryDescriptor(admission: ZLinkClientServerAdmission) {
  return {
    channelName: admission.channelName,
    serverRoutingId: admission.serverRid,
    lifecycleGeneration: admission.lifecycleGeneration,
    descriptorRevision: admission.descriptorRevision,
    weight: admission.weight,
    state: discoveryAvailabilityForRuntimeState(admission.state),
    securityIdentity: admission.securityIdentity,
    effectiveMaxMessageBytes: admission.normalizedEffectiveMaxMessageBytes,
    advertisedEndpoint: admission.advertisedEndpoint
  };
}

function sameClientServerDiscoveryDescriptor(
  left: ClientServerDiscoveryDescriptor,
  right: ClientServerDiscoveryDescriptor
): boolean {
  return (
    left.channelName === right.channelName &&
    left.serverRoutingId === right.serverRoutingId &&
    left.lifecycleGeneration === right.lifecycleGeneration &&
    left.descriptorRevision === right.descriptorRevision &&
    left.weight === right.weight &&
    left.state === right.state &&
    left.securityIdentity === right.securityIdentity &&
    left.effectiveMaxMessageBytes === right.effectiveMaxMessageBytes &&
    left.advertisedEndpoint === right.advertisedEndpoint
  );
}

function closeMessages(parts: readonly Message[]): void {
  for (const part of parts) part.close();
}

function setFanoutSubscriptions(
  subscriber: ZLinkBackendSubscriberSocket,
  applicationTopics: readonly string[] | undefined
): void {
  const topics = new Set(applicationTopics);
  if (topics.size === 0) topics.add('');
  topics.add(FANOUT_LIVENESS_TOPIC);
  for (const topic of topics) subscriber.setSubscription(topic);
}

function deriveRoutingId(baseRoutingId: string, suffix: string): string {
  const derived = `${baseRoutingId}\0${suffix}`;
  if (Buffer.byteLength(derived, 'utf8') > ZLINK_MAX_ROUTING_ID_BYTES) {
    throw new ZLinkConfigurationException(
      `Derived routing id with suffix '${suffix}' exceeds the ${ZLINK_MAX_ROUTING_ID_BYTES} byte limit.`
    );
  }
  return derived;
}

function applyFanoutPublisherSocketOptions(
  publisher: ZLinkBackendPublisherSocket,
  channel: ZLinkChannelOptions
): void {
  publisher.noDrop = channel.noDrop ?? false;
  publisher.sendTimeoutMs = configuredSendTimeoutMs(channel.publisher?.sendTimeoutMs);
}

function fanoutDiscoveryConnectionId(connectionId: string): string {
  return `fanout:${connectionId.replaceAll('\0', '/')}`;
}

function newLifecycleGeneration(): bigint {
  for (;;) {
    const value = randomBytes(8).readBigUInt64BE() & MAX_LIFECYCLE_GENERATION;
    if (value !== 0n) return value;
  }
}

function advertisedEndpoint(boundEndpoint: string, advertiseHost: string | undefined): string {
  const result = buildAdvertisedEndpoint(boundEndpoint, advertiseHost, 'tcp');
  if (result === undefined) {
    throw new ZLinkConfigurationException(
      `ClientServer advertised host requires a TCP endpoint, received '${boundEndpoint}'.`
    );
  }
  return result;
}

function requirePublicWeight(weight: number): void {
  if (!isValidPublicWeight(weight)) {
    throw new ZLinkConfigurationException(
      `Weight must be an integer in 0..${ZLINK_MAX_PUBLIC_WEIGHT}.`
    );
  }
}

function rawAvailabilityWeight(weight: number): number {
  // Core raw peer weight remains a transport availability signal in 0..100.
  // Framework descriptors and selectors retain the exact public weight.
  return weight === 0 ? 0 : 100;
}

function applySocketConfig(
  socket: ZLinkBackendDealerSocket | ZLinkBackendRouterSocket,
  config: {
    readonly sendHighWaterMark?: number;
    readonly receiveHighWaterMark?: number;
    readonly receiveTimeoutMs?: number;
    readonly maxMessageSize?: number;
  }
): void {
  if (config.sendHighWaterMark !== undefined) {
    socket.sendHighWaterMark = config.sendHighWaterMark;
  }
  if (config.receiveHighWaterMark !== undefined) {
    socket.receiveHighWaterMark = config.receiveHighWaterMark;
  }
  if (config.receiveTimeoutMs !== undefined) {
    socket.receiveTimeoutMs = config.receiveTimeoutMs;
  }
  if (config.maxMessageSize !== undefined) {
    socket.maxMessageSize = config.maxMessageSize;
  }
}

function configuredSendTimeoutMs(value: number | undefined): number {
  return value ?? DEFAULT_SEND_TIMEOUT_MS;
}
