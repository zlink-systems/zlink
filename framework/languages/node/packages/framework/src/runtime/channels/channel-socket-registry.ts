import {
  ZLinkFrameworkRuntimeState,
  type RoutingId,
  type ZLinkClientServerServerDescriptor,
  type ZLinkFanoutPublisherDescriptor
} from '../../contracts';
import { ZLinkSocketNativeEventType } from '../diagnostics/internal-event-contracts';
import {
  ZLinkConfigurationException,
  type ZLinkFrameworkRegistration
} from '../configuration';
import type {
  ZLinkBackendContext,
  ZLinkBackendDealerSocket,
  ZLinkBackendPublisherSocket,
  ZLinkBackendRouterSocket,
  ZLinkBackendReadablePoller,
  ZLinkBackendSocketMonitor,
  ZLinkBackendSocketMonitorEvent,
  ZLinkBackendSubscriberSocket,
  ZLinkChannelBackendAdapter,
  ZLinkMonitoringBackendAdapter
} from '../backend/contracts';
import { ZLinkAsyncSubmitter } from '../messaging';
import { throwIfAborted } from '../abort';
import { ZLinkRouteDisconnectedError } from './route-disconnected-error';
import { attachEndpointConnections } from '../../contracts/Configuration/RuntimeEndpointConnections';
import { ZLinkRouteMemberSnapshot } from './route-member-snapshot';
import { randomBytes, randomUUID } from 'node:crypto';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { Message } from '../../contracts/Common/Message';
import { ServiceDiscoveryRegistry } from '../foundation/service-discovery-registry';
import {
  decodeClientServerControl,
  encodeClientServerAdmit,
  encodeClientServerHello,
  encodeClientServerLivenessAck,
  encodeClientServerLivenessProbe,
  encodeClientServerReject,
  encodeClientServerUpdate,
  isClientServerControlFrame,
  type ZLinkClientServerAdmission
} from './client-server-service-wire';
import {
  FANOUT_LIVENESS_PAYLOAD,
  FANOUT_LIVENESS_TOPIC,
  inspectFanoutInbound
} from './fanout-service-wire';
import type { ZLinkChannelEnvelopeHeader } from './channel-envelope';

const MAX_LIFECYCLE_GENERATION = 0x7fff_ffff_ffff_ffffn;
const CLIENT_SERVER_PROBE_INTERVAL_MS = 5_000;
const CLIENT_SERVER_PEER_DEADLINE_MS = 15_000;
const CLIENT_SERVER_READY_WAIT_CAP_MS = 5_000;
const CLIENT_SERVER_READY_POLL_INTERVAL_MS = 5;
const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;

export interface ZLinkClientServerServerSocketIdentity {
  readonly serverRid: string;
  readonly lifecycleGeneration: bigint;
  readonly endpoint: string;
}

export interface ZLinkClientServerConnectionCallbacks {
  readonly onTransportReady: (routingId: string, endpoint: string) => void;
  readonly onTerminated: (routingId: string | undefined, endpoint: string) => void;
  readonly reconnectOnTermination?: boolean;
}

type ClientServerDiscoveryDescriptor =
  Parameters<ServiceDiscoveryRegistry['admitClientServer']>[0];

interface ClientServerPhysicalConnection {
  readonly channelName: string;
  readonly endpoint: string;
  readonly dealer: ZLinkBackendDealerSocket;
  readonly readablePoller: ZLinkBackendReadablePoller;
  readonly monitor: ZLinkBackendSocketMonitor;
  readonly aliases: Set<string>;
  readonly callbacksByAlias: Map<string, ZLinkClientServerConnectionCallbacks>;
  physicalConnectionId: symbol;
  readyConnectionId?: string;
  admittedDescriptor?: ClientServerDiscoveryDescriptor;
  nextProbeAt?: number;
  deadlineAt?: number;
  outstandingProbeId?: bigint;
  admissionAttempt?: symbol;
}

export interface ZLinkFanoutConnectionCallbacks {
  readonly onReady: () => void;
  readonly onTerminated: (reason: 'disconnect' | 'deadline' | 'protocol') => void;
}

interface FanoutPublisherConnection {
  readonly channelName: string;
  readonly endpoint: string;
  readonly subscriber: ZLinkBackendSubscriberSocket;
  readonly monitor: ZLinkBackendSocketMonitor;
  readonly callbacks: ZLinkFanoutConnectionCallbacks;
  readonly physicalConnectionId: symbol;
  transportReady: boolean;
  ready: boolean;
  deadlineAt?: number;
}

export class ZLinkChannelSocketRegistry {
  private readonly clientDealers = new Map<string, ZLinkBackendDealerSocket>();
  private readonly channelRouters = new Map<string, ZLinkBackendRouterSocket>();
  private readonly publishers = new Map<string, ZLinkBackendPublisherSocket>();
  private readonly routeRouters = new Map<string, ZLinkBackendRouterSocket>();
  private readonly routeMembers = new ZLinkRouteMemberSnapshot();
  private readonly clientServerIdentities = new Map<string, {
    readonly serverRid: string;
    readonly lifecycleGeneration: bigint;
  }>();
  private readonly clientServerConnections =
    new Map<string, ClientServerPhysicalConnection>();
  private readonly clientServerDiscovery = new ServiceDiscoveryRegistry();
  private readonly clientServerReadyIdentities = new Map<string, {
    readonly channelName: string;
    readonly serverRoutingId: string;
    readonly lifecycleGeneration: bigint;
  }>();
  private readonly clientServerServerDescriptors =
    new Map<string, ZLinkClientServerServerDescriptor>();
  private readonly clientServerPublicWeights = new Map<string, number>();
  private readonly routeMeshPublicWeights = new Map<string, number>();
  private readonly clientServerAdmittedClients = new Map<string, Set<RoutingId>>();
  private readonly clientServerServerPeers = new Map<string, {
    readonly channelName: string;
    readonly routingId: RoutingId;
    readonly physicalConnectionId: symbol;
    nextProbeAt: number;
    deadlineAt: number;
    outstandingProbeId?: bigint;
  }>();
  private readonly submitters = new WeakMap<object, ZLinkAsyncSubmitter>();
  private readonly ownedSubmitters = new Set<ZLinkAsyncSubmitter>();
  private readonly ownedMonitors = new Set<ZLinkBackendSocketMonitor>();
  private readonly fanoutConnections =
    new Map<string, FanoutPublisherConnection>();
  private readonly fanoutPublisherNextBeacon = new Map<string, number>();
  private clientServerLivenessTimer?: NodeJS.Timeout;
  private nextClientServerProbeId = 1n;
  private readonly clientServerMonitorHandlers =
    new Map<string, Set<(event: ZLinkBackendSocketMonitorEvent) => void>>();
  private readonly fanoutMonitorHandlers =
    new Map<string, Set<(event: ZLinkBackendSocketMonitorEvent) => void>>();
  private readonly fanoutTopologyHandlers =
    new Map<string, Set<() => void>>();

  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly adapter: ZLinkChannelBackendAdapter,
    private readonly context: ZLinkBackendContext,
    private readonly monitoringAdapter?: ZLinkMonitoringBackendAdapter,
    private readonly oneWayFailureSink?: (error: unknown) => void
  ) {}

  async dispose(): Promise<void> {
    const sockets = [
      ...this.clientDealers.values(),
      ...[...new Set(this.clientServerConnections.values())].map(value => value.dealer),
      ...[...this.fanoutConnections.values()].map(value => value.subscriber),
      ...this.channelRouters.values(),
      ...this.publishers.values(),
      ...this.routeRouters.values()
    ];
    this.clientDealers.clear();
    this.clientServerConnections.clear();
    this.clientServerReadyIdentities.clear();
    this.clientServerServerDescriptors.clear();
    this.clientServerPublicWeights.clear();
    this.routeMeshPublicWeights.clear();
    this.clientServerAdmittedClients.clear();
    this.clientServerServerPeers.clear();
    this.clientServerMonitorHandlers.clear();
    this.fanoutMonitorHandlers.clear();
    this.fanoutTopologyHandlers.clear();
    this.fanoutConnections.clear();
    this.fanoutPublisherNextBeacon.clear();
    if (this.clientServerLivenessTimer !== undefined) {
      clearInterval(this.clientServerLivenessTimer);
      this.clientServerLivenessTimer = undefined;
    }
    this.channelRouters.clear();
    this.publishers.clear();
    this.routeRouters.clear();
    this.routeMembers.clear();
    this.disposeSubmitters();
    const monitors = [...this.ownedMonitors];
    this.ownedMonitors.clear();
    const cleanup = await Promise.allSettled([
      ...monitors.map((monitor) => monitor.dispose()),
      ...sockets.map((socket) => socket.dispose())
    ]);
    const errors = cleanup
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Channel socket cleanup failed.');
  }

  disposeSubmitters(): void {
    for (const submitter of this.ownedSubmitters) {
      submitter.dispose();
    }
    this.ownedSubmitters.clear();
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
    dealer.setChannelName(channelName);
    if (channel?.routingId !== undefined && channel.routingId.length > 0) {
      dealer.setRoutingId(deriveRoutingId(channel.routingId, 'dealer'));
    }
    applySocketConfig(dealer, client);
    this.trackSubmitter(dealer);
    this.clientDealers.set(channelName, dealer);
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
      throw new ZLinkConfigurationException(`Channel server '${channelName}' does not define a bind endpoint.`);
    }

    const router = this.adapter.createRouterSocket(this.context);
    router.setChannelName(channelName);
    const identity = this.clientServerIdentities.get(channelName) ?? {
      serverRid: channel.server.routingId
        ?? `${channel.routingIdPrefix ?? 'cs'}-${randomUUID()}`,
      lifecycleGeneration: newLifecycleGeneration()
    };
    this.clientServerIdentities.set(channelName, identity);
    router.setRoutingId(identity.serverRid);
    const publicWeight = channel.server.weight ?? 100;
    this.clientServerPublicWeights.set(channelName, publicWeight);
    router.peerWeight = rawAvailabilityWeight(publicWeight);
    applySocketConfig(router, channel.server);
    this.trackSubmitter(router);
    router.bind(channel.server.bind);
    if (this.monitoringAdapter !== undefined) {
      const monitor = this.monitoringAdapter.openSocketMonitor(router);
      this.ownedMonitors.add(monitor);
      monitor.onEvent((event) => {
        if (event.routingId === undefined) return;
        if (event.nativeEvent !== ZLinkSocketNativeEventType.Disconnected
          && event.nativeEvent !== ZLinkSocketNativeEventType.Closed
          && event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedNoDetail
          && event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedProtocol
          && event.nativeEvent !== ZLinkSocketNativeEventType.HandshakeFailedAuth) return;
        const routingId = String(event.routingId);
        this.clientServerServerPeers.delete(clientServerServerPeerKey(channelName, routingId));
        this.clientServerAdmittedClients.get(channelName)?.delete(routingId);
      });
    }
    this.channelRouters.set(channelName, router);
    if (!this.clientServerServerDescriptors.has(channelName)) {
      const boundEndpoint = router.lastEndpoint ?? channel.server.bind;
      this.clientServerServerDescriptors.set(channelName, {
        channelName,
        serverRid: identity.serverRid,
        lifecycleGeneration: identity.lifecycleGeneration,
        descriptorRevision: 1n,
        endpoint: advertisedEndpoint(boundEndpoint, channel.server.advertiseHost),
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
    const router = this.channelRouter(channelName);
    const identity = this.clientServerIdentities.get(channelName);
    const channel = this.registration.channels.get(channelName);
    if (identity === undefined || channel?.server === undefined) {
      throw new ZLinkConfigurationException(`Channel server '${channelName}' is not registered.`);
    }
    const boundEndpoint = router.lastEndpoint ?? channel.server.bind;
    if (boundEndpoint === undefined || boundEndpoint.length === 0) {
      throw new ZLinkConfigurationException(
        `Channel server '${channelName}' did not report its bound endpoint.`
      );
    }
    return {
      ...identity,
      endpoint: advertisedEndpoint(boundEndpoint, channel.server.advertiseHost)
    };
  }

  clientServerServerSocket(channelName: string): ZLinkBackendRouterSocket {
    return this.channelRouter(channelName);
  }

  clientServerServerWeight(channelName: string): number {
    this.channelRouter(channelName);
    return this.clientServerPublicWeights.get(channelName) ?? 100;
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
    dealer.setChannelName(channelName);
    dealer.setRoutingId(`cs-client-${randomUUID()}`);
    applySocketConfig(dealer, client);
    this.trackSubmitter(dealer);
    const cleanupDealer = (): void => {
      const submitter = this.submitters.get(dealer);
      if (submitter !== undefined) {
        submitter.dispose();
        this.ownedSubmitters.delete(submitter);
        this.submitters.delete(dealer);
      }
      void Promise.allSettled([dealer.dispose()]);
    };
    let readablePoller: ZLinkBackendReadablePoller;
    try {
      readablePoller = this.adapter.createReadablePoller(dealer);
    } catch (error) {
      cleanupDealer();
      throw error;
    }
    let monitor: ZLinkBackendSocketMonitor;
    try {
      monitor = this.monitoringAdapter.openSocketMonitor(dealer);
    } catch (error) {
      try {
        readablePoller.dispose();
      } finally {
        cleanupDealer();
      }
      throw error;
    }
    this.ownedMonitors.add(monitor);
    const connection: ClientServerPhysicalConnection = {
      channelName,
      endpoint,
      dealer,
      readablePoller,
      monitor,
      aliases: new Set([connectionId]),
      callbacksByAlias: new Map([[connectionId, callbacks]]),
      physicalConnectionId: Symbol(connectionId)
    };
    this.clientServerConnections.set(connectionId, connection);
    this.ensureClientServerLivenessTimer();
    try {
      monitor.onEvent((event) => {
        if (![...connection.aliases].some(
          alias => this.clientServerConnections.get(alias) === connection
        )) return;
        for (const handler of this.clientServerMonitorHandlers.get(channelName) ?? []) {
          handler(event);
        }
        const routingId = event.routingId === undefined ? undefined : String(event.routingId);
        if (event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady) {
          connection.physicalConnectionId = Symbol(connectionId);
          connection.admissionAttempt = undefined;
          for (const currentCallbacks of [...connection.callbacksByAlias.values()]) {
            currentCallbacks.onTransportReady(routingId ?? '', event.remoteAddr);
          }
          return;
        }
        if (event.nativeEvent === ZLinkSocketNativeEventType.Disconnected
          || event.nativeEvent === ZLinkSocketNativeEventType.Closed
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedNoDetail
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedProtocol
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedAuth) {
          connection.physicalConnectionId = Symbol(connectionId);
          connection.admissionAttempt = undefined;
          if (connection.readyConnectionId !== undefined) {
            this.removeReadyConnection(connection.readyConnectionId);
          }
          for (const currentCallbacks of [...connection.callbacksByAlias.values()]) {
            currentCallbacks.onTerminated(routingId, event.remoteAddr);
          }
        }
      });
      dealer.connect(endpoint);
    } catch (error) {
      this.clientServerConnections.delete(connectionId);
      this.ownedMonitors.delete(monitor);
      try {
        readablePoller.dispose();
      } finally {
        cleanupDealer();
        void Promise.allSettled([monitor.dispose()]);
      }
      throw error;
    }
    return dealer;
  }

  async closeClientServerConnection(connectionId: string): Promise<void> {
    const current = this.clientServerConnections.get(connectionId);
    if (current === undefined) return;
    this.clientServerConnections.delete(connectionId);
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
      if (replacement !== undefined && current.admittedDescriptor !== undefined
        && this.clientServerDiscovery.admitClientServer(
          current.admittedDescriptor,
          replacement
        )) {
        current.readyConnectionId = replacement;
        this.clientServerReadyIdentities.set(replacement, {
          channelName: current.admittedDescriptor.channelName,
          serverRoutingId: current.admittedDescriptor.serverRoutingId,
          lifecycleGeneration: current.admittedDescriptor.lifecycleGeneration
        });
      }
    }
    if (current.aliases.size > 0) return;
    await this.disposeClientServerPhysical(connectionId, current);
  }

  async disconnectClientServerConnection(connectionId: string): Promise<void> {
    const current = this.clientServerConnections.get(connectionId);
    if (current === undefined) return;
    this.clientServerConnections.delete(connectionId);
    current.aliases.delete(connectionId);
    current.callbacksByAlias.delete(connectionId);
    this.clientServerReadyIdentities.delete(connectionId);
    if (current.aliases.size > 0) return;
    await this.disposeClientServerPhysical(connectionId, current, false);
  }

  private async disposeClientServerPhysical(
    connectionId: string,
    current: ClientServerPhysicalConnection,
    disconnectTransport = true
  ): Promise<void> {
    if (disconnectTransport) {
      try {
        current.dealer.disconnect(current.endpoint);
      } catch {
        // Disposal below is the terminal cleanup if the transport already closed.
      }
    }
    const submitter = this.submitters.get(current.dealer);
    submitter?.rejectActive(new ZLinkRouteDisconnectedError(
      `ClientServer connection '${connectionId}' closed.`
    ));
    if (submitter !== undefined) {
      submitter.dispose();
      this.ownedSubmitters.delete(submitter);
      this.submitters.delete(current.dealer);
    }
    this.ownedMonitors.delete(current.monitor);
    current.readablePoller.dispose();
    const results = await Promise.allSettled([
      current.monitor.dispose(),
      current.dealer.dispose()
    ]);
    const errors = results
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map(result => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `ClientServer connection '${connectionId}' cleanup failed.`);
    }
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
      return admitted;
    }
    const duplicateId = [...this.clientServerReadyIdentities]
      .find(([, identity]) =>
        identity.channelName === descriptor.channelName
        && identity.serverRoutingId === descriptor.serverRoutingId
        && identity.lifecycleGeneration === descriptor.lifecycleGeneration)?.[0];
    if (duplicateId !== undefined) {
      const shared = this.clientServerConnections.get(duplicateId);
      if (shared !== undefined && shared !== connection) {
        this.clientServerConnections.set(connectionId, shared);
        shared.aliases.add(connectionId);
        const callbacks = connection.callbacksByAlias.get(connectionId);
        if (callbacks !== undefined) shared.callbacksByAlias.set(connectionId, callbacks);
        connection.aliases.clear();
        connection.callbacksByAlias.clear();
        void this.disposeClientServerPhysical(connectionId, connection)
          .catch(error => this.oneWayFailureSink?.(error));
        return true;
      }
    }
    const admitted = this.clientServerDiscovery.admitClientServer(descriptor, connectionId);
    if (admitted) {
      const now = performance.now();
      connection.nextProbeAt = now + CLIENT_SERVER_PROBE_INTERVAL_MS;
      connection.deadlineAt = now + CLIENT_SERVER_PEER_DEADLINE_MS;
      connection.outstandingProbeId = undefined;
      this.clientServerReadyIdentities.set(connectionId, {
        channelName: descriptor.channelName,
        serverRoutingId: descriptor.serverRoutingId,
        lifecycleGeneration: descriptor.lifecycleGeneration
      });
      connection.readyConnectionId = connectionId;
      connection.admittedDescriptor = descriptor;
    }
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
        connection.deadlineAt = undefined;
        connection.nextProbeAt = undefined;
        connection.outstandingProbeId = undefined;
      }
    }
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
      throw new ZLinkConfigurationException(`Channel client '${channelName}' is not registered.`);
    }
    return this.selectClientServerDealer(channelName);
  }

  /**
   * Resolves a ClientServer send target, waiting when the ready candidate set is still empty, as
   * `framework/doc/framework/common/spec/08-channel-messaging.ko.md` §3.2 requires: the call waits
   * at call time for a bounded period and then fails with no-target. The bound is the shorter of
   * this Channel's request timeout and five seconds, mirroring the .NET reference
   * `ZLinkClientServerClientRuntime.WaitForReadyAsync`. The wait lives here because this registry
   * owns ClientServer connections, admission and weighted selection.
   *
   * The wait only observes admission that is already in flight: it never opens or reconnects a
   * connection, so it cannot trigger the admission it waits for, and Framework startup keeps not
   * waiting for local ClientServer admission. It applies whenever nothing is selectable, including
   * candidate sets excluded by weight 0 or drain, because `selectClientServerDealer` filters the
   * same way the reference `SelectReady()` does.
   *
   * Every attempt yields to the event loop between polls. ClientServer admission completes on the
   * monitor callbacks that the backend poll timer pumps, so a synchronous wait would starve the
   * admission it is waiting for and burn the whole bound before failing anyway. That is this
   * execution model's form of the hazard the JVM mirror avoids by not holding the admission
   * monitor across its sleep.
   */
  async awaitClientDealerForOutbound(
    channelName: string,
    signal?: AbortSignal
  ): Promise<ZLinkBackendDealerSocket | undefined> {
    const deadline = Date.now() + this.clientServerReadyWaitBoundMs(channelName);
    for (;;) {
      const dealer = this.clientDealerForOutbound(channelName);
      if (dealer !== undefined) return dealer;
      if (Date.now() >= deadline) return undefined;
      throwIfAborted(signal);
      await new Promise<void>(resolve => {
        setTimeout(resolve, CLIENT_SERVER_READY_POLL_INTERVAL_MS);
      });
    }
  }

  private clientServerReadyWaitBoundMs(channelName: string): number {
    const requestTimeoutMs = this.registration.channels.get(channelName)?.requestTimeoutMs
      ?? this.registration.requestTimeoutMs
      ?? DEFAULT_REQUEST_TIMEOUT_MS;
    return Math.min(requestTimeoutMs, CLIENT_SERVER_READY_WAIT_CAP_MS);
  }

  startManualClientServerConnections(): void {
    for (const [channelName, channel] of this.registration.channels) {
      const client = channel.client;
      if (client === undefined) continue;
      const endpoints = channel.client?.manualConnections ?? [];
      if (endpoints.length === 0) continue;
      const open = (endpoint: string) => this.openManualClientServerConnection(channelName, endpoint);
      const close = (endpoint: string) => {
        void this.closeClientServerConnection(manualClientServerConnectionId(channelName, endpoint))
          .catch(error => this.oneWayFailureSink?.(error));
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

  clientServerMonitoringSource(channelName: string): ZLinkBackendSocketMonitor {
    if (this.registration.channels.get(channelName)?.client === undefined) {
      throw new ZLinkConfigurationException(`Channel client '${channelName}' is not registered.`);
    }
    let disposed = false;
    let handler: ((event: ZLinkBackendSocketMonitorEvent) => void) | undefined;
    return {
      nativeInstance: {},
      onEvent: (next) => {
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
      },
      recv: () => {
        throw new ZLinkConfigurationException(
          'ClientServer aggregate monitoring supports event callbacks only.'
        );
      },
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
  ): readonly Parameters<ServiceDiscoveryRegistry['admitClientServer']>[0][] {
    return this.clientServerDiscovery.clientServerDescriptors(channelName);
  }

  hasKnownClientServerTargets(channelName: string): boolean {
    return this.clientServerDiscovery.clientServerDescriptors(channelName).length > 0;
  }

  fanoutActiveTargets(channelName: string) {
    return this.clientServerDiscovery.fanoutEndpoints(channelName);
  }

  admitFanoutPublisher(
    descriptor: ZLinkFanoutPublisherDescriptor,
    connectionId: string
  ): boolean {
    const connection = this.fanoutConnections.get(connectionId);
    if (connection === undefined || !connection.ready) return false;
    const admitted = this.clientServerDiscovery.admitFanoutPublisher({
      channelName: descriptor.channelName,
      publisherRoutingId: String(descriptor.publisherRid),
      lifecycleGeneration: descriptor.lifecycleGeneration,
      descriptorRevision: descriptor.descriptorRevision,
      advertisedEndpoint: descriptor.endpoint,
      state: runtimeStateName(descriptor.state)
    }, fanoutDiscoveryConnectionId(connectionId));
    this.notifyFanoutTopology(descriptor.channelName);
    return admitted;
  }

  removeFanoutPublisher(
    descriptor: ZLinkFanoutPublisherDescriptor,
    connectionId: string
  ): boolean {
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
  }

  tryHandleClientServerControl(
    channelName: string,
    received: {
      readonly parts: readonly Message[];
      readonly requestSeq: bigint | null;
      readonly routingId: unknown;
    },
    router: ZLinkBackendRouterSocket
  ): boolean {
    if (received.parts.length === 0) return false;
    const first = received.parts[0];
    if (!isClientServerControlFrame(first.data())) return false;
    let reply: Buffer;
    try {
      const record = decodeClientServerControl(first.data());
      if (record.kind === 'livenessProbe'
        && received.parts.length === 1
        && received.requestSeq !== null) {
        reply = encodeClientServerLivenessAck(record.probeId);
      } else if (record.kind === 'livenessAck'
        && received.parts.length === 1
        && received.requestSeq === null) {
        this.acceptClientServerServerLivenessAck(
          channelName,
          String(received.routingId),
          record.probeId
        );
        return true;
      } else if (record.kind !== 'hello'
        || received.parts.length !== 1
        || received.requestSeq === null) {
        reply = encodeClientServerReject(1);
      } else {
        const descriptor = this.clientServerServerDescriptors.get(channelName);
        if (descriptor === undefined
          || record.hello.channelName !== channelName
          || record.hello.securityIdentity !== descriptor.securityIdentity) {
          reply = encodeClientServerReject(3);
        } else {
          reply = encodeClientServerAdmit(
            descriptor,
            normalizedMessageLimit(router.maxMessageSize)
          );
          this.admitClientServerServerPeer(
            channelName,
            String(received.routingId)
          );
        }
      }
    } catch {
      reply = encodeClientServerReject(1);
    }
    if (received.requestSeq !== null) {
      const message = RuntimeMessage.from(reply);
      try {
        router.reply(received.routingId as RoutingId, received.requestSeq, message);
      } finally {
        message.close();
      }
    }
    return true;
  }

  tickClientServerLiveness(nowMs = performance.now()): void {
    for (const connection of new Set(this.clientServerConnections.values())) {
      const connectionId = connection.readyConnectionId
        ?? connection.aliases.values().next().value as string | undefined;
      if (connectionId === undefined) continue;
      this.drainClientServerControl(connectionId, connection);
      if (connection.deadlineAt === undefined) continue;
      if (nowMs >= connection.deadlineAt) {
        this.removeReadyConnection(connectionId);
        const callbacks = [...connection.callbacksByAlias.values()];
        for (const currentCallbacks of callbacks) {
          currentCallbacks.onTerminated(undefined, connection.endpoint);
        }
        if (callbacks.some(value => value.reconnectOnTermination === true)
          && [...connection.aliases].some(
            alias => this.clientServerConnections.get(alias) === connection
          )) {
          try {
            connection.dealer.disconnect(connection.endpoint);
            connection.dealer.connect(connection.endpoint);
          } catch (error) {
            this.oneWayFailureSink?.(error);
          }
        }
        continue;
      }
      if (connection.nextProbeAt === undefined || nowMs < connection.nextProbeAt) continue;
      connection.nextProbeAt = nowMs + CLIENT_SERVER_PROBE_INTERVAL_MS;
      const probeId = connection.outstandingProbeId ?? this.allocateClientServerProbeId();
      connection.outstandingProbeId = probeId;
      this.requestClientServerLiveness(connectionId, connection, probeId);
    }

    for (const [key, peer] of [...this.clientServerServerPeers]) {
      if (nowMs >= peer.deadlineAt) {
        this.clientServerServerPeers.delete(key);
        this.clientServerAdmittedClients.get(peer.channelName)?.delete(peer.routingId);
        try {
          this.channelRouters.get(peer.channelName)?.disconnectPeer(peer.routingId);
        } catch (error) {
          this.oneWayFailureSink?.(error);
        }
        continue;
      }
      if (nowMs < peer.nextProbeAt) continue;
      peer.nextProbeAt = nowMs + CLIENT_SERVER_PROBE_INTERVAL_MS;
      const probeId = peer.outstandingProbeId ?? this.allocateClientServerProbeId();
      peer.outstandingProbeId = probeId;
      this.requestClientServerServerLiveness(peer, probeId);
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
      throw new ZLinkConfigurationException(
        `Fanout connection '${connectionId}' is already open.`
      );
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
    subscriber.setChannelName(channelName);
    subscriber.setSubscription('');
    subscriber.setSubscription(FANOUT_LIVENESS_TOPIC);
    const monitor = this.monitoringAdapter.openSocketMonitor(subscriber);
    const connection: FanoutPublisherConnection = {
      channelName,
      endpoint,
      subscriber,
      monitor,
      callbacks,
      physicalConnectionId: Symbol(connectionId),
      transportReady: false,
      ready: false
    };
    this.fanoutConnections.set(connectionId, connection);
    this.ownedMonitors.add(monitor);
    this.ensureClientServerLivenessTimer();
    try {
      monitor.onEvent((event) => {
        if (this.fanoutConnections.get(connectionId) !== connection) return;
        for (const handler of this.fanoutMonitorHandlers.get(channelName) ?? []) {
          handler(event);
        }
        if (event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady) {
          connection.transportReady = true;
          return;
        }
        if (event.nativeEvent === ZLinkSocketNativeEventType.Disconnected
          || event.nativeEvent === ZLinkSocketNativeEventType.Closed
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedNoDetail
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedProtocol
          || event.nativeEvent === ZLinkSocketNativeEventType.HandshakeFailedAuth) {
          connection.transportReady = false;
          connection.ready = false;
          connection.deadlineAt = undefined;
          callbacks.onTerminated('disconnect');
        }
      });
      subscriber.connect(endpoint);
    } catch (error) {
      this.fanoutConnections.delete(connectionId);
      this.ownedMonitors.delete(monitor);
      void Promise.allSettled([monitor.dispose(), subscriber.dispose()]);
      throw error;
    }
    return subscriber;
  }

  async closeFanoutSubscriberConnection(
    connectionId: string
  ): Promise<void> {
    const connection = this.fanoutConnections.get(connectionId);
    if (connection === undefined) return;
    this.fanoutConnections.delete(connectionId);
    try {
      connection.subscriber.disconnect(connection.endpoint);
    } catch {
      // Disposal remains the terminal cleanup path.
    }
    this.ownedMonitors.delete(connection.monitor);
    const results = await Promise.allSettled([
      connection.monitor.dispose(),
      connection.subscriber.dispose()
    ]);
    const errors = results
      .filter((result): result is PromiseRejectedResult =>
        result.status === 'rejected')
      .map(result => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(
        errors,
        `Fanout connection '${connectionId}' cleanup failed.`
      );
    }
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
    return this.handleFanoutInboundResult(
      connectionId,
      topicMessage,
      source,
      nowMs
    ).consumed;
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
    const classification = inspectFanoutInbound(
      topicMessage.topic,
      topicMessage.parts
    );
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
    let handler:
      ((event: ZLinkBackendSocketMonitorEvent) => void) | undefined;
    let disposed = false;
    return {
      nativeInstance: {},
      onEvent: next => {
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
      recv: () => {
        throw new ZLinkConfigurationException(
          'Aggregated fanout monitoring is callback-only.'
        );
      },
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
      onChange: next => {
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

  private notifyFanoutTopology(channelName: string): void {
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
      this.fanoutPublisherNextBeacon.set(
        channelName,
        nowMs + CLIENT_SERVER_PROBE_INTERVAL_MS
      );
      const payload = RuntimeMessage.from(FANOUT_LIVENESS_PAYLOAD);
      try {
        publisher.publish(FANOUT_LIVENESS_TOPIC, payload, 0);
      } finally {
        payload.close();
      }
    }
    for (const [connectionId, connection] of [...this.fanoutConnections]) {
      if (connection.deadlineAt === undefined
        || nowMs < connection.deadlineAt) continue;
      connection.ready = false;
      connection.deadlineAt = undefined;
      connection.callbacks.onTerminated('deadline');
      if (this.fanoutConnections.get(connectionId) === connection) {
        void this.closeFanoutSubscriberConnection(connectionId)
          .catch(error => this.oneWayFailureSink?.(error));
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
        void this.admitConfiguredClientServerConnection(channelName, connectionId)
          .catch(error => {
            this.removeReadyConnection(connectionId);
            this.oneWayFailureSink?.(error);
          });
      },
      onTerminated: () => this.removeReadyConnection(connectionId),
      reconnectOnTermination: true
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
    try {
      const admission = await requestClientServerAdmission(
        connection.dealer,
        channelName,
        'default',
        15_000
      );
      if (this.clientServerConnections.get(connectionId) !== connection
        || connection.physicalConnectionId !== physicalConnectionId
        || connection.admissionAttempt !== admissionAttempt) return;
      if (admission.channelName !== channelName
        || admission.securityIdentity !== 'default') {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission does not match its configured identity.`
        );
      }
      if (!this.admitClientServerConnection(
        admissionToDiscoveryDescriptor(admission),
        connectionId
      )) {
        throw new ZLinkConfigurationException(
          `ClientServer '${channelName}' admission was stale.`
        );
      }
    } finally {
      if (this.clientServerConnections.get(connectionId) === connection
        && connection.admissionAttempt === admissionAttempt) {
        connection.admissionAttempt = undefined;
      }
    }
  }

  private drainClientServerControl(
    connectionId: string,
    connection: {
      readonly channelName: string;
      readonly dealer: ZLinkBackendDealerSocket;
      readonly physicalConnectionId: symbol;
      readonly readablePoller: ZLinkBackendReadablePoller;
    }
  ): void {
    for (;;) {
      if (!connection.readablePoller.wait(0)) return;
      const received = connection.dealer.recv(1);
      if (received === undefined) return;
      try {
        if (received.parts.length !== 1
          || !isClientServerControlFrame(received.parts[0]!.data())) {
          throw new ZLinkConfigurationException(
            `ClientServer '${connection.channelName}' received an unsolicited application frame.`
          );
        }
        const record = decodeClientServerControl(received.parts[0]!.data());
        if (record.kind === 'livenessProbe') {
          const ack = RuntimeMessage.from(encodeClientServerLivenessAck(record.probeId));
          try {
            if (!connection.dealer.send(ack, 0)) {
              throw new ZLinkConfigurationException(
                `ClientServer '${connection.channelName}' liveness ACK was not submitted.`
              );
            }
          } finally {
            ack.close();
          }
          continue;
        }
        if (record.kind !== 'update') {
          throw new ZLinkConfigurationException(
            `ClientServer '${connection.channelName}' received an invalid pushed control record.`
          );
        }
        this.applyClientServerDescriptorUpdate(connectionId, connection, record.admission);
      } catch (error) {
        this.removeReadyConnection(connectionId);
        this.oneWayFailureSink?.(error);
      } finally {
        received.close();
      }
    }
  }

  private applyClientServerDescriptorUpdate(
    connectionId: string,
    connection: { readonly channelName: string; readonly physicalConnectionId: symbol },
    admission: ZLinkClientServerAdmission
  ): void {
    const currentConnection = this.clientServerConnections.get(connectionId);
    if (currentConnection === undefined
      || currentConnection.physicalConnectionId !== connection.physicalConnectionId) return;
    const identity = this.clientServerReadyIdentities.get(connectionId);
    if (identity === undefined) return;
    const current = this.clientServerDiscovery.clientServerDescriptors(connection.channelName)
      .find(value =>
        value.serverRoutingId === identity.serverRoutingId);
    if (current === undefined) return;
    if (admission.channelName !== current.channelName
      || admission.serverRid !== current.serverRoutingId
      || admission.lifecycleGeneration !== current.lifecycleGeneration
      || admission.securityIdentity !== current.securityIdentity
      || admission.advertisedEndpoint !== current.advertisedEndpoint) {
      throw new ZLinkConfigurationException(
        `ClientServer '${connection.channelName}' descriptor update changed immutable identity.`
      );
    }
    const candidate = admissionToDiscoveryDescriptor(admission);
    if (candidate.descriptorRevision < current.descriptorRevision) return;
    if (candidate.descriptorRevision === current.descriptorRevision) {
      if (!sameClientServerDiscoveryDescriptor(candidate, current)) {
        throw new ZLinkConfigurationException(
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
      100
    );
    this.clientServerLivenessTimer.unref();
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
      connection.deadlineAt = undefined;
      connection.nextProbeAt = undefined;
      connection.outstandingProbeId = undefined;
    }
  }

  private requestClientServerLiveness(
    connectionId: string,
    connection: {
      readonly dealer: ZLinkBackendDealerSocket;
      readonly physicalConnectionId: symbol;
      outstandingProbeId?: bigint;
      deadlineAt?: number;
    },
    probeId: bigint
  ): void {
    const message = RuntimeMessage.from(encodeClientServerLivenessProbe(probeId));
    const submitted = connection.dealer.request(message, (result, parts) => {
      try {
        const current = this.clientServerConnections.get(connectionId);
        if (current === undefined
          || current.physicalConnectionId !== connection.physicalConnectionId
          || current.outstandingProbeId !== probeId
          || result !== 0
          || parts.length !== 1) return;
        const record = decodeClientServerControl(parts[0]!.data());
        if (record.kind !== 'livenessAck' || record.probeId !== probeId) return;
        current.outstandingProbeId = undefined;
        current.deadlineAt = performance.now() + CLIENT_SERVER_PEER_DEADLINE_MS;
      } finally {
        message.close();
        closeMessages(parts);
      }
    }, 0, CLIENT_SERVER_PEER_DEADLINE_MS);
    if (!submitted) message.close();
  }

  private admitClientServerServerPeer(channelName: string, routingId: RoutingId): void {
    const key = clientServerServerPeerKey(channelName, routingId);
    const now = performance.now();
    const peer = {
      channelName,
      routingId,
      physicalConnectionId: Symbol(key),
      nextProbeAt: now + CLIENT_SERVER_PROBE_INTERVAL_MS,
      deadlineAt: now + CLIENT_SERVER_PEER_DEADLINE_MS
    };
    this.clientServerServerPeers.set(key, peer);
    let clients = this.clientServerAdmittedClients.get(channelName);
    if (clients === undefined) {
      clients = new Set();
      this.clientServerAdmittedClients.set(channelName, clients);
    }
    clients.add(routingId);
    this.ensureClientServerLivenessTimer();
  }

  private requestClientServerServerLiveness(
    peer: {
      readonly channelName: string;
      readonly routingId: RoutingId;
      readonly physicalConnectionId: symbol;
      outstandingProbeId?: bigint;
      deadlineAt: number;
    },
    probeId: bigint
  ): void {
    const router = this.channelRouters.get(peer.channelName);
    if (router === undefined) return;
    const message = RuntimeMessage.from(encodeClientServerLivenessProbe(probeId));
    try {
      router.send(peer.routingId, message, 0);
    } finally {
      message.close();
    }
  }

  private acceptClientServerServerLivenessAck(
    channelName: string,
    routingId: RoutingId,
    probeId: bigint
  ): void {
    const peer = this.clientServerServerPeers.get(
      clientServerServerPeerKey(channelName, routingId)
    );
    if (peer === undefined || peer.outstandingProbeId !== probeId) return;
    peer.outstandingProbeId = undefined;
    peer.deadlineAt = performance.now() + CLIENT_SERVER_PEER_DEADLINE_MS;
  }

  private pushClientServerDescriptorUpdate(
    channelName: string,
    descriptor: ZLinkClientServerServerDescriptor
  ): void {
    const router = this.channelRouters.get(channelName);
    const clients = this.clientServerAdmittedClients.get(channelName);
    if (router === undefined || clients === undefined) return;
    for (const routingId of [...clients]) {
      const message = RuntimeMessage.from(encodeClientServerUpdate(
        descriptor,
        normalizedMessageLimit(router.maxMessageSize)
      ));
      try {
        if (!router.send(routingId, message, 0)) clients.delete(routingId);
      } finally {
        message.close();
      }
    }
  }

  private allocateClientServerProbeId(): bigint {
    const result = this.nextClientServerProbeId;
    this.nextClientServerProbeId = result === 0xffff_ffff_ffff_ffffn ? 1n : result + 1n;
    return result;
  }

  publisher(channelName: string): ZLinkBackendPublisherSocket {
    const existing = this.publishers.get(channelName);
    if (existing !== undefined) {
      return existing;
    }

    const channel = this.registration.channels.get(channelName);
    if (channel?.publisher === undefined) {
      throw new ZLinkConfigurationException(`Channel publisher '${channelName}' is not registered.`);
    }
    if (channel.publisher.bind === undefined) {
      throw new ZLinkConfigurationException(`Channel publisher '${channelName}' does not define a bind endpoint.`);
    }

    const publisher = this.adapter.createPublisherSocket(this.context);
    publisher.setChannelName(channelName);
    this.trackSubmitter(publisher);
    publisher.bind(channel.publisher.bind);
    this.publishers.set(channelName, publisher);
    this.fanoutPublisherNextBeacon.set(
      channelName,
      performance.now() + CLIENT_SERVER_PROBE_INTERVAL_MS
    );
    this.ensureClientServerLivenessTimer();
    return publisher;
  }

  fanoutPublisherEndpoint(channelName: string): string | undefined {
    const channel = this.registration.channels.get(channelName);
    const publisher = this.publishers.get(channelName);
    const boundEndpoint = publisher?.lastEndpoint;
    if (channel?.publisher === undefined || boundEndpoint === undefined || boundEndpoint.length === 0) {
      return undefined;
    }
    return advertisedEndpoint(boundEndpoint, channel.publisher.advertiseHost);
  }

  routeRouter(routerChannelId: string): ZLinkBackendRouterSocket {
    const existing = this.routeRouters.get(routerChannelId);
    if (existing !== undefined) {
      return existing;
    }

    const routeChannel = this.registration.routeChannelOptions.get(routerChannelId);
    if (routeChannel === undefined) {
      throw new ZLinkConfigurationException(`Route channel '${routerChannelId}' is not registered.`);
    }
    const router = this.adapter.createRouterSocket(this.context);
    router.setChannelName(routerChannelId);
    const routerOptions: unknown = 'options' in router ? router.options : undefined;
    if (
      (routeChannel.manualConnections?.length ?? 0) > 0 &&
      typeof routerOptions === 'object' &&
      routerOptions !== null &&
      'probe' in routerOptions
    ) {
      (routerOptions as { probe: boolean }).probe = true;
    }
    router.setRoutingId(
      routeChannel.routingId
      ?? `${routeChannel.routingIdPrefix ?? routerChannelId}-${randomUUID()}`
    );
    const publicWeight = routeChannel.weight ?? 100;
    this.routeMeshPublicWeights.set(routerChannelId, publicWeight);
    router.peerWeight = rawAvailabilityWeight(publicWeight);
    applySocketConfig(router, routeChannel);
    this.trackSubmitter(router);
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
    this.routeRouters.set(routerChannelId, router);
    return router;
  }

  routeMeshSocket(routerChannelId: string): ZLinkBackendRouterSocket {
    return this.routeRouter(routerChannelId);
  }

  routeMeshWeight(routerChannelId: string): number {
    this.routeRouter(routerChannelId);
    return this.routeMeshPublicWeights.get(routerChannelId) ?? 100;
  }

  setRouteMeshWeight(routerChannelId: string, weight: number): void {
    requirePublicWeight(weight);
    const router = this.routeRouter(routerChannelId);
    this.routeMeshPublicWeights.set(routerChannelId, weight);
    router.peerWeight = rawAvailabilityWeight(weight);
  }

  requireSubmitter(socket: ZLinkBackendDealerSocket | ZLinkBackendPublisherSocket | ZLinkBackendRouterSocket): ZLinkAsyncSubmitter {
    const submitter = this.submitters.get(socket);
    if (submitter === undefined) {
      throw new ZLinkConfigurationException('Channel submit runtime is not started.');
    }
    return submitter;
  }

  routeMemberStatus(routerChannelId: string, targetNodeRid: string): 'unknown' | 'missing' | 'connected' | 'disconnected' {
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
    this.ownedMonitors.add(monitor);
    monitor.onEvent((event) => {
      if (event.nativeEvent === ZLinkSocketNativeEventType.Disconnected && event.remoteAddr.length > 0) {
        handler(event.remoteAddr);
      }
    });
  }

  private trackSubmitter(socket: ZLinkBackendDealerSocket | ZLinkBackendPublisherSocket | ZLinkBackendRouterSocket): void {
    const submitter = new ZLinkAsyncSubmitter((handler) => socket.onSendReady(handler), {
      ...('sendTimeoutMs' in socket && socket.sendTimeoutMs !== -1
        ? { timeoutMs: socket.sendTimeoutMs }
        : {}),
      capacity: Math.max(1, socket.sendHighWaterMark),
      onCommandFailure: this.oneWayFailureSink
    });
    this.submitters.set(socket, submitter);
    this.ownedSubmitters.add(submitter);
  }

  private trackRouteMonitor(routerChannelId: string, router: ZLinkBackendRouterSocket): void {
    if (this.monitoringAdapter === undefined) {
      return;
    }
    const monitor = this.monitoringAdapter.openSocketMonitor(router);
    this.ownedMonitors.add(monitor);
    monitor.onEvent((event) => {
      const routingId = event.routingId === undefined ? undefined : String(event.routingId);
      if (event.nativeEvent === ZLinkSocketNativeEventType.ConnectionReady && routingId !== undefined) {
        this.routeMembers.observeReady(routerChannelId, routingId, event.remoteAddr);
        return;
      }
      if (
        event.nativeEvent !== ZLinkSocketNativeEventType.Disconnected
        && event.nativeEvent !== ZLinkSocketNativeEventType.Closed
      ) return;
      this.routeMembers.observeTermination(routerChannelId, routingId, event.remoteAddr);
      const submitter = this.submitters.get(router);
      submitter?.rejectActive(new ZLinkRouteDisconnectedError(
        `Route channel '${routerChannelId}' disconnected: ${event.nativeEvent}/${event.value}`
      ));
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

function requestClientServerAdmission(
  dealer: ZLinkBackendDealerSocket,
  channelName: string,
  securityIdentity: string,
  timeoutMs: number
): Promise<ZLinkClientServerAdmission> {
  const message = RuntimeMessage.from(encodeClientServerHello({
    channelName,
    securityIdentity,
    normalizedEffectiveMaxMessageBytes: normalizedMessageLimit(dealer.maxMessageSize)
  }));
  return new Promise((resolve, reject) => {
    let submitted = false;
    try {
      submitted = dealer.request(message, (result, parts) => {
        try {
          if (result !== 0 || parts.length !== 1) {
            reject(new ZLinkConfigurationException(
              `ClientServer '${channelName}' admission request failed with result ${result}.`
            ));
            return;
          }
          const record = decodeClientServerControl(parts[0]!.data());
          if (record.kind === 'reject') {
            reject(new ZLinkConfigurationException(
              `ClientServer '${channelName}' admission was rejected (${record.reason}).`
            ));
            return;
          }
          if (record.kind !== 'admit') {
            reject(new ZLinkConfigurationException(
              `ClientServer '${channelName}' admission reply has an invalid command.`
            ));
            return;
          }
          resolve(record.admission);
        } catch (error) {
          reject(error);
        } finally {
          message.close();
          closeMessages(parts);
        }
      }, 0, timeoutMs);
    } catch (error) {
      message.close();
      reject(error);
      return;
    }
    if (!submitted) {
      message.close();
      reject(new ZLinkConfigurationException(
        `ClientServer '${channelName}' admission request was not submitted.`
      ));
    }
  });
}

function admissionToDiscoveryDescriptor(admission: ZLinkClientServerAdmission) {
  return {
    channelName: admission.channelName,
    serverRoutingId: admission.serverRid,
    lifecycleGeneration: admission.lifecycleGeneration,
    descriptorRevision: admission.descriptorRevision,
    weight: admission.weight,
    state: runtimeStateName(admission.state),
    securityIdentity: admission.securityIdentity,
    effectiveMaxMessageBytes: admission.normalizedEffectiveMaxMessageBytes,
    advertisedEndpoint: admission.advertisedEndpoint
  };
}

function runtimeStateName(
  state: ZLinkFrameworkRuntimeState
): 'preparing' | 'serving' | 'retiring' | 'stopped' | 'error' {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing: return 'preparing';
    case ZLinkFrameworkRuntimeState.Serving: return 'serving';
    case ZLinkFrameworkRuntimeState.Relocating:
    case ZLinkFrameworkRuntimeState.Relocated:
    case ZLinkFrameworkRuntimeState.Draining: return 'retiring';
    case ZLinkFrameworkRuntimeState.Stopped: return 'stopped';
    default: return 'error';
  }
}

function sameClientServerDiscoveryDescriptor(
  left: ClientServerDiscoveryDescriptor,
  right: ClientServerDiscoveryDescriptor
): boolean {
  return left.channelName === right.channelName
    && left.serverRoutingId === right.serverRoutingId
    && left.lifecycleGeneration === right.lifecycleGeneration
    && left.descriptorRevision === right.descriptorRevision
    && left.weight === right.weight
    && left.state === right.state
    && left.securityIdentity === right.securityIdentity
    && left.effectiveMaxMessageBytes === right.effectiveMaxMessageBytes
    && left.advertisedEndpoint === right.advertisedEndpoint;
}

function closeMessages(parts: readonly Message[]): void {
  for (const part of parts) part.close();
}

function deriveRoutingId(baseRoutingId: string, suffix: string): string {
  const derived = `${baseRoutingId}\0${suffix}`;
  if (Buffer.byteLength(derived, 'utf8') > 255) {
    throw new ZLinkConfigurationException(
      `Derived routing id with suffix '${suffix}' exceeds the 255 byte limit.`
    );
  }
  return derived;
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
  if (advertiseHost === undefined) return boundEndpoint;
  const match = /^tcp:\/\/(?:\[[^\]]+\]|[^:]+):(\d+)$/.exec(boundEndpoint);
  if (match === null) {
    throw new ZLinkConfigurationException(
      `ClientServer advertised host requires a TCP endpoint, received '${boundEndpoint}'.`
    );
  }
  const host = advertiseHost.includes(':') && !advertiseHost.startsWith('[')
    ? `[${advertiseHost}]`
    : advertiseHost;
  return `tcp://${host}:${match[1]}`;
}

function normalizedMessageLimit(value: number): number {
  return Number.isSafeInteger(value) && value > 0
    ? Math.min(value, 0xffff_ffff)
    : 0x7fff_ffff;
}

function requirePublicWeight(weight: number): void {
  if (!Number.isInteger(weight) || weight < 0 || weight > 10_000) {
    throw new ZLinkConfigurationException('Weight must be an integer in 0..10000.');
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
    readonly sendTimeoutMs?: number;
    readonly maxMessageSize?: number;
  }
): void {
  if (config.sendHighWaterMark !== undefined) {
    socket.sendHighWaterMark = config.sendHighWaterMark;
  }
  if (config.receiveHighWaterMark !== undefined) {
    socket.receiveHighWaterMark = config.receiveHighWaterMark;
  }
  if (config.sendTimeoutMs !== undefined) {
    socket.sendTimeoutMs = config.sendTimeoutMs;
  }
  if (config.maxMessageSize !== undefined) {
    socket.maxMessageSize = config.maxMessageSize;
  }
}
