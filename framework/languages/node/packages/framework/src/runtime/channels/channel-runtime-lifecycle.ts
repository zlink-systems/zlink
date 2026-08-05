import type { ZLinkLocationOptionOverrides } from '../../contracts/Locations/Options';
import type { ZLinkRequestHandler, ZLinkSendHandler } from '../../contracts/Handlers';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type {
  ZLinkBackendRouterSocket,
  ZLinkBackendSpotRouteBridge,
  ZLinkChannelBackendAdapter
} from '../backend/contracts';
import {
  ZLINK_BACKEND_SPOT_ROUTE_BRIDGE_ROUTE_WITH_CHANNEL_INBOUND
} from '../backend/contracts';
import type { ZLinkRuntimeTaskRunner } from '../execution';
import {
  ZLinkLocationRuntime,
  type ZLinkLocationEventSink,
  type ZLinkLocationRuntimeStores
} from '../locations';
import type { Message } from '../../contracts/Common/Message';
import type { ReceiveRecord } from '../foundation/service-runtime-contracts';
import type { RoutingId } from '../../contracts';
import {
  createChannelLocationAutoConnectContext,
  type ZLinkChannelLocationAutoConnectContext
} from './channel-autoconnect';
import {
  collectRouteChannelHandlers,
  ZLinkChannelPublishDispatcher,
  ZLinkChannelRequestDispatcher,
  ZLinkRoutePacketDispatcher,
  type ZLinkRouteHandlerRegistration,
  type ZLinkRouteRuntimeRequestHandler,
  type ZLinkRouteRuntimeSendHandler
} from './channel-dispatchers';
import {
  ZLinkChannelReceiveLoop,
  ZLinkReceiveRoundRobinCoordinator,
  ZLinkRouteReceiveLoop,
  ZLinkSubscriberReceiveLoop
} from './channel-receive-loops';
import { ZLinkChannelDispatchServices } from './channel-dispatch-services';
import type { ZLinkChannelEnvelopeCodecRegistry } from './channel-envelope';
import { ZLinkChannelSocketRegistry } from './channel-socket-registry';
import { ZLinkSpotRouteBridgeRawReplyRegistry } from './spot-route-bridge-raw-reply';
import { ZLinkSpotRouteDispatchStrategy } from './spot-route-dispatch-strategy';
import { ZLinkClientServerLocationRuntime } from './client-server-location-runtime';
import { ZLinkFanoutLocationRuntime } from './fanout-location-runtime';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';
import {
  attachEndpointConnections,
  detachEndpointConnections,
  endpointConnections
} from '../../contracts/Configuration/RuntimeEndpointConnections';

interface ManualFanoutSubscriberState {
  readonly channelName: string;
  readonly endpoint: string;
  readonly index: number;
  readonly connectionId: string;
  readonly taskRunner: ZLinkRuntimeTaskRunner;
  readonly dispatcher: ZLinkChannelPublishDispatcher;
  desired: boolean;
  transition: Promise<void>;
  openingToken?: symbol;
  active?: ManualFanoutSubscriberActive;
  stopping?: ManualFanoutSubscriberActive;
}

interface ManualFanoutSubscriberActive {
  readonly token: symbol;
  readonly loop: ZLinkSubscriberReceiveLoop;
}

export interface ZLinkChannelRuntimeLifecycleOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly adapter: ZLinkChannelBackendAdapter;
  readonly sockets: ZLinkChannelSocketRegistry;
  readonly codecs: ZLinkChannelEnvelopeCodecRegistry;
  readonly dispatchServices: ZLinkChannelDispatchServices;
  readonly spotRoutes: ZLinkSpotRouteDispatchStrategy;
  readonly spotRouteBridges: Map<string, ZLinkBackendSpotRouteBridge>;
  readonly spotRouteBridgeRawReplies: ZLinkSpotRouteBridgeRawReplyRegistry;
  readonly internalRouteSendHandlers?: ReadonlyMap<string, ZLinkRouteRuntimeSendHandler>;
  readonly internalRouteRequestHandlers?: ReadonlyMap<string, ZLinkRouteRuntimeRequestHandler>;
  readonly inboundDispatchBudget?: ZLinkInboundDispatchBudget;
}

export class ZLinkChannelRuntimeLifecycle {
  private readonly receiveRoundRobin = new ZLinkReceiveRoundRobinCoordinator();
  private readonly channelReceiveLoops: ZLinkChannelReceiveLoop[] = [];
  private readonly subscriberReceiveLoops: ZLinkSubscriberReceiveLoop[] = [];
  private readonly manualFanoutSubscribers = new Map<string, ManualFanoutSubscriberState>();
  private readonly manualFanoutSubscriberOwners = new Set<object>();
  private readonly routeReceiveLoops: Array<{ stop(): Promise<void> }> = [];
  private readonly meshChannelDispatchers = new Map<string, ZLinkChannelRequestDispatcher>();
  private readonly meshRouteDispatchers = new Map<string, ZLinkRoutePacketDispatcher>();
  private nextManualFanoutIndex = 0;
  private disposed = false;
  private locationAutoConnect?: ZLinkChannelLocationAutoConnectContext;
  private clientServerLocation?: ZLinkClientServerLocationRuntime;
  private fanoutLocation?: ZLinkFanoutLocationRuntime;
  private taskRunner?: ZLinkRuntimeTaskRunner;

  constructor(private readonly options: ZLinkChannelRuntimeLifecycleOptions) {}

  configureLocationAutoConnect(
    runtime: ZLinkLocationRuntime,
    stores: ZLinkLocationRuntimeStores,
    options: ZLinkLocationOptionOverrides,
    events?: ZLinkLocationEventSink
  ): void {
    this.locationAutoConnect = createChannelLocationAutoConnectContext(runtime, stores, options, events);
  }

  async startLocationAutoConnect(signal?: AbortSignal): Promise<void> {
    const location = this.locationAutoConnect;
    if (location === undefined
      || this.clientServerLocation !== undefined
      || this.fanoutLocation !== undefined) {
      return;
    }

    try {
      if (hasClientServerLocationTopology(this.options.registration)) {
        this.clientServerLocation = new ZLinkClientServerLocationRuntime(
          this.options.registration,
          this.options.sockets,
          location.runtime,
          location.stores,
          location.options
        );
        await this.clientServerLocation.start(signal);
      }
      if (hasFanoutLocationTopology(this.options.registration)) {
        this.fanoutLocation = new ZLinkFanoutLocationRuntime(
          this.options.registration,
          this.options.sockets,
          location.runtime,
          location.stores,
          location.options,
          (channelName, connectionId, subscriber) =>
            this.startFanoutSubscriberLoop(
              channelName,
              connectionId,
              subscriber
            )
        );
        await this.fanoutLocation.start(signal);
      }
    } catch (error) {
      await this.clientServerLocation?.stop(signal).catch(() => undefined);
      this.clientServerLocation = undefined;
      await this.fanoutLocation?.stop(signal).catch(() => undefined);
      this.fanoutLocation = undefined;
      throw error;
    }
  }

  async reclaimLocationOwnerRows(signal?: AbortSignal): Promise<void> {
    await this.clientServerLocation?.reclaimOwnerRows(signal);
    await this.fanoutLocation?.reclaimOwnerRows(signal);
  }

  bindRouteMeshRouters(): void {
    for (const routeChannel of this.options.registration.routeChannelOptions.values()) {
      if (routeChannel.bind !== undefined && routeChannel.bind.trim().length > 0) {
        this.options.sockets.routeRouter(routeChannel.routerChannelId);
      }
    }
  }

  start(taskRunner?: ZLinkRuntimeTaskRunner): Promise<void>[] {
    this.taskRunner = taskRunner;
    this.prepareMeshDispatch(taskRunner);
    this.openOutboundSockets();
    const channelReceivers = this.startChannelReceivers(taskRunner);
    const subscriberReceivers = this.startSubscriberReceivers(taskRunner);
    const routeReceivers = this.startRouteReceivers(taskRunner);
    const tasks = [
      ...channelReceivers,
      ...subscriberReceivers,
      ...routeReceivers
    ];
    return tasks;
  }

  prepareMeshDispatch(taskRunner?: ZLinkRuntimeTaskRunner): void {
    if (this.meshChannelDispatchers.size > 0) {
      return;
    }
    if (taskRunner === undefined) {
      throw new ZLinkConfigurationException('MeshNode channel dispatch requires a runtime task runner.');
    }
    for (const [meshName, mesh] of this.options.registration.spotNodes) {
      const routeHandlers: ZLinkRouteHandlerRegistration[] = [
        ...(mesh.routeSendHandlers ?? []).map((registration) => ({
          kind: 'send' as const,
          packetName: registration.packetName,
          handler: this.options.dispatchServices.routeSendHandler(registration.handlerType)
        })),
        ...(mesh.routeRequestHandlers ?? []).map((registration) => ({
          kind: 'request' as const,
          packetName: registration.packetName,
          handler: this.options.dispatchServices.routeRequestHandler(registration.handlerType)
        })),
        ...[...this.options.internalRouteSendHandlers?.entries() ?? []].map(
          ([packetName, handler]): ZLinkRouteHandlerRegistration => ({ kind: 'send', packetName, handler })
        ),
        ...[...this.options.internalRouteRequestHandlers?.entries() ?? []].map(
          ([packetName, handler]): ZLinkRouteHandlerRegistration => ({ kind: 'request', packetName, handler })
        )
      ];
      if (routeHandlers.length > 0) {
        this.meshRouteDispatchers.set(meshName, new ZLinkRoutePacketDispatcher({
          routerChannelId: meshName,
          codecs: this.options.codecs,
          dispatchErrors: this.options.dispatchServices.dispatchErrorReporter(taskRunner.errorSink),
          handlers: routeHandlers,
          filters: this.options.dispatchServices.handlerFilters(),
          unhandled: this.options.registration.dispatch?.unhandled
        }));
      }
      for (const [channelName, channel] of Object.entries(mesh.meshChannels ?? {})) {
        if ((channel.requestHandlers ?? []).length === 0 && (channel.sendHandlers ?? []).length === 0) {
          continue;
        }
        this.meshChannelDispatchers.set(meshChannelKey(meshName, channelName), new ZLinkChannelRequestDispatcher({
          meshName,
          channelName,
          routeMesh: true,
          codecs: this.options.codecs,
          dispatchErrors: this.options.dispatchServices.dispatchErrorReporter(taskRunner.errorSink),
          handlers: new Map(channel.requestHandlers?.map((handler) => [
            handler.packetName,
            (handler as typeof handler & { readonly handler?: ZLinkRequestHandler<unknown, unknown> }).handler
              ?? adaptRouteRequestHandler(
                this.options.dispatchServices.routeRequestHandler(handler.handlerType)
              )
          ])),
          sendHandlers: new Map(channel.sendHandlers?.map((handler) => [
            handler.packetName,
            (handler as typeof handler & { readonly handler?: ZLinkSendHandler<unknown> }).handler
              ?? adaptRouteSendHandler(
                this.options.dispatchServices.routeSendHandler(handler.handlerType)
              )
          ])),
          filters: this.options.dispatchServices.handlerFilters(),
          unhandled: this.options.registration.dispatch?.unhandled
        }));
      }
    }
  }

  dispatchMeshChannel(meshName: string, record: ReceiveRecord, signal?: AbortSignal): Promise<void> {
    const channelName = record.channelName;
    if (channelName === null) {
      throw new ZLinkConfigurationException('MeshNode channel record is missing channelName.');
    }
    const dispatcher = this.meshChannelDispatchers.get(meshChannelKey(meshName, channelName));
    if (dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${meshName}' channel '${channelName}' has no registered handlers.`
      );
    }
    return dispatcher.dispatchMesh(record, signal);
  }

  dispatchMeshRoute(
    meshName: string,
    record: ReceiveRecord,
    signal?: AbortSignal
  ): Promise<void> {
    const dispatcher = this.meshRouteDispatchers.get(meshName);
    if (dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${meshName}' has no registered node-direct handlers.`
      );
    }
    return dispatcher.dispatchMesh(record, signal);
  }

  dispatchLocalMeshRoute(
    meshName: string,
    sourceNodeRid: RoutingId,
    parts: readonly Message[],
    signal?: AbortSignal
  ): Promise<void> {
    const dispatcher = this.meshRouteDispatchers.get(meshName);
    if (dispatcher === undefined) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${meshName}' has no registered node-direct handlers.`
      );
    }
    return dispatcher.dispatchLocalCommand(parts, sourceNodeRid, signal);
  }

  canDispatchLocalMeshRoute(meshName: string): boolean {
    return this.meshRouteDispatchers.has(meshName);
  }

  async dispose(signal?: AbortSignal): Promise<void> {
    const channelLoops = [...this.channelReceiveLoops];
    const manualStates = [...this.manualFanoutSubscribers.values()];
    this.disposed = true;
    for (const state of manualStates) state.desired = false;
    for (const owner of this.manualFanoutSubscriberOwners) {
      detachEndpointConnections(owner);
    }
    this.manualFanoutSubscriberOwners.clear();
    this.manualFanoutSubscribers.clear();
    const manualLoops = new Set(
      manualStates
        .map(state => state.active?.loop ?? state.stopping?.loop)
        .filter((loop): loop is ZLinkSubscriberReceiveLoop => loop !== undefined)
    );
    const subscriberLoops = [...this.subscriberReceiveLoops]
      .filter(loop => !manualLoops.has(loop));
    const manualTransitions = manualStates.map(state => this.drainManualFanoutSubscriber(state));
    const routeLoops = [...this.routeReceiveLoops];
    const clientServerLocation = this.clientServerLocation;
    const fanoutLocation = this.fanoutLocation;
    const spotRouteBridges = [...this.options.spotRouteBridges.values()];
    this.channelReceiveLoops.length = 0;
    this.subscriberReceiveLoops.length = 0;
    this.routeReceiveLoops.length = 0;
    this.clientServerLocation = undefined;
    this.fanoutLocation = undefined;
    this.taskRunner = undefined;
    this.meshChannelDispatchers.clear();
    this.meshRouteDispatchers.clear();
    this.options.spotRouteBridges.clear();
    const loopStops = [
      ...channelLoops.map((loop) => loop.stop()),
      ...subscriberLoops.map((loop) => loop.stop()),
      ...routeLoops.map((loop) => loop.stop())
    ];
    const transportStopped = await Promise.allSettled([
      ...loopStops,
      ...manualTransitions
    ]);
    const descriptorStopped = clientServerLocation === undefined
      ? []
      : await Promise.allSettled([clientServerLocation.stop(signal)]);
    const fanoutDescriptorStopped = fanoutLocation === undefined
      ? []
      : await Promise.allSettled([fanoutLocation.stop(signal)]);
    this.options.spotRouteBridgeRawReplies.rejectAll(
      new ZLinkConfigurationException('Channel runtime disposed before the SPOT route reply arrived.')
    );
    const cleanup = await Promise.allSettled([
      ...spotRouteBridges.map((bridge) => bridge.dispose()),
      this.options.sockets.dispose()
    ]);
    const errors = [
      ...transportStopped,
      ...descriptorStopped,
      ...fanoutDescriptorStopped,
      ...cleanup
    ]
      .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
      .map((result) => result.reason);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Channel runtime cleanup failed.');
  }

  private openOutboundSockets(): void {
    // The process-local Server is an independent peer source. Location Store
    // discovery may later alias the same admitted identity, but startup and
    // fail-static selection must not depend on descriptor publication.
    this.options.sockets.startLocalClientServerConnections();
    this.options.sockets.startManualClientServerConnections();
    for (const channelName of this.options.registration.fanoutPublishers) {
      this.options.sockets.publisher(channelName);
    }
  }

  private startChannelReceivers(taskRunner: ZLinkRuntimeTaskRunner | undefined): Promise<void>[] {
    const tasks: Promise<void>[] = [];
    for (const [channelName, channel] of this.options.registration.channels) {
      if (
        channel.server?.bind === undefined ||
        ((channel.requestHandlers ?? []).length === 0 && (channel.sendHandlers ?? []).length === 0)
      ) {
        continue;
      }
      if (taskRunner === undefined) {
        throw new ZLinkConfigurationException(
          `Channel '${channelName}' handler dispatch requires a runtime task runner.`
        );
      }
      const router = this.options.sockets.channelRouter(channelName);
      const dispatcher = new ZLinkChannelRequestDispatcher({
        channelName,
        codecs: this.options.codecs,
        dispatchErrors: this.options.dispatchServices.dispatchErrorReporter(taskRunner.errorSink),
        handlers: new Map(channel.requestHandlers?.map((handler) => [
          handler.packetName,
          handler.handler ?? this.options.dispatchServices.channelRequestHandler(
            requireChannelHandlerType(handler.handlerType, channelName, handler.packetName)
          )
        ])),
        sendHandlers: new Map(channel.sendHandlers?.map((handler) => [
          handler.packetName,
          handler.handler ?? this.options.dispatchServices.channelSendHandler(
            requireChannelHandlerType(handler.handlerType, channelName, handler.packetName)
          )
        ])),
        filters: this.options.dispatchServices.handlerFilters(),
        unhandled: this.options.registration.dispatch?.unhandled,
        replySubmitter: this.options.sockets.requireSubmitter(router)
      });
      const spotRouteBridge = this.createSpotRouteBridgeForRouter(channelName, router);
      const loop = new ZLinkChannelReceiveLoop(
        channelName,
        router,
        dispatcher,
        spotRouteBridge,
        (received, socket) =>
          this.options.sockets.tryHandleClientServerControl(channelName, received, socket),
        this.options.inboundDispatchBudget,
        this.options.adapter.createReadablePoller(router),
        error => taskRunner.errorSink.reportRuntimeTaskException(
          `channel:${channelName}:dispatch`,
          error
        ),
        this.receiveRoundRobin
      );
      this.channelReceiveLoops.push(loop);
      tasks.push(taskRunner.run(`channel:${channelName}`, (signal) => loop.run(signal)));
    }
    return tasks;
  }

  private startSubscriberReceivers(taskRunner: ZLinkRuntimeTaskRunner | undefined): Promise<void>[] {
    const tasks: Promise<void>[] = [];
    for (const [channelName, channel] of this.options.registration.channels) {
      if (channel.subscriber === undefined || (channel.publishHandlers ?? []).length === 0) {
        continue;
      }
      if (taskRunner === undefined) {
        throw new ZLinkConfigurationException(
          `Fanout channel '${channelName}' publish handler dispatch requires a runtime task runner.`
        );
      }
      const dispatcher = new ZLinkChannelPublishDispatcher({
        channelName,
        codecs: this.options.codecs,
        dispatchErrors: this.options.dispatchServices.dispatchErrorReporter(taskRunner.errorSink),
        handlers: new Map(channel.publishHandlers?.map((handler) => [handler.packetName, handler.handler])),
        filters: this.options.dispatchServices.handlerFilters(),
        unhandled: this.options.registration.dispatch?.unhandled,
        metrics: this.options.dispatchServices.metrics()
      });
      const endpointHandle = endpointConnections(
        channel.subscriber,
        channel.subscriber.manualConnections ?? []
      );
      const endpoints = endpointHandle.listConnections();
      this.manualFanoutSubscriberOwners.add(channel.subscriber);
      attachEndpointConnections(channel.subscriber, {
        connect: endpoint => this.requestManualFanoutConnect(
          channelName,
          endpoint,
          taskRunner,
          dispatcher
        ),
        disconnect: endpoint => this.requestManualFanoutDisconnect(
          channelName,
          endpoint
        )
      });
      for (const endpoint of endpoints) {
        const state = this.getOrCreateManualFanoutSubscriber(
          channelName,
          endpoint,
          this.nextManualFanoutIndex++,
          taskRunner,
          dispatcher
        );
        tasks.push(this.openManualFanoutSubscriber(state));
      }
    }
    return tasks;
  }

  private getOrCreateManualFanoutSubscriber(
    channelName: string,
    endpoint: string,
    index: number,
    taskRunner: ZLinkRuntimeTaskRunner,
    dispatcher: ZLinkChannelPublishDispatcher
  ): ManualFanoutSubscriberState {
    const key = manualFanoutSubscriberKey(channelName, endpoint);
    const existing = this.manualFanoutSubscribers.get(key);
    if (existing !== undefined) {
      existing.desired = true;
      return existing;
    }
    const state: ManualFanoutSubscriberState = {
      channelName,
      endpoint,
      index,
      connectionId: key,
      taskRunner,
      dispatcher,
      desired: true,
      transition: Promise.resolve()
    };
    this.manualFanoutSubscribers.set(key, state);
    return state;
  }

  private requestManualFanoutConnect(
    channelName: string,
    endpoint: string,
    taskRunner: ZLinkRuntimeTaskRunner,
    dispatcher: ZLinkChannelPublishDispatcher
  ): void {
    if (this.disposed) return;
    const state = this.getOrCreateManualFanoutSubscriber(
      channelName,
      endpoint,
      this.nextManualFanoutIndex++,
      taskRunner,
      dispatcher
    );
    state.desired = true;
    this.enqueueManualFanoutTransition(state, async () => {
      if (!state.desired || state.active !== undefined || this.taskRunner !== taskRunner) return;
      void this.openManualFanoutSubscriber(state).catch(error => taskRunner.errorSink
        .reportRuntimeTaskException(
          `subscriber:${state.channelName}:manual:${state.index}:open`,
          error
        ));
    });
  }

  private requestManualFanoutDisconnect(channelName: string, endpoint: string): void {
    const state = this.manualFanoutSubscribers.get(manualFanoutSubscriberKey(channelName, endpoint));
    if (state === undefined) return;
    state.desired = false;
    this.enqueueManualFanoutTransition(state, async () => {
      try {
        await this.closeManualFanoutSubscriber(state);
      } finally {
        if (!state.desired) this.manualFanoutSubscribers.delete(state.connectionId);
      }
    });
  }

  private enqueueManualFanoutTransition(
    state: ManualFanoutSubscriberState,
    transition: () => Promise<void> | void
  ): void {
    state.transition = state.transition
      .then(transition, transition)
      .catch(error => state.taskRunner.errorSink.reportRuntimeTaskException(
        `subscriber:${state.channelName}:manual:${state.index}:lifecycle`,
        error
      ));
  }

  private openManualFanoutSubscriber(state: ManualFanoutSubscriberState): Promise<void> {
    const { channelName, endpoint, connectionId, taskRunner, dispatcher } = state;
    if (this.disposed || !state.desired || this.taskRunner !== taskRunner) {
      return Promise.resolve();
    }
    const token = Symbol(`manual-fanout:${channelName}:${endpoint}`);
    state.openingToken = token;
    let loop: ZLinkSubscriberReceiveLoop;
    let reconnecting = false;
    try {
      const subscriber = this.options.sockets.openFanoutSubscriberConnection(
        channelName,
        connectionId,
        endpoint,
        {
          onReady() {},
          onTerminated: () => {
            if (this.disposed
              || (state.active?.token !== token && state.openingToken !== token)
              || reconnecting) return;
            reconnecting = true;
            setImmediate(() => {
              if (this.disposed
                || (state.active?.token !== token && state.openingToken !== token)) {
                reconnecting = false;
                return;
              }
              this.enqueueManualFanoutTransition(state, async () => {
                reconnecting = false;
                if (this.disposed || state.active?.token !== token) return;
                try {
                  await this.closeManualFanoutSubscriber(state, token);
                } catch (error) {
                  taskRunner.errorSink.reportRuntimeTaskException(
                    `subscriber:${state.channelName}:manual:${state.index}:cleanup`,
                    error
                  );
                }
                if (this.shouldReconnectManualFanoutSubscriber(state)) {
                  void this.openManualFanoutSubscriber(state).catch(error => taskRunner.errorSink
                    .reportRuntimeTaskException(
                      `subscriber:${state.channelName}:manual:${state.index}:reconnect`,
                      error
                    ));
                }
              });
            });
          }
        }
      );
      loop = new ZLinkSubscriberReceiveLoop(
        this.options.adapter,
        subscriber,
        dispatcher,
        message => this.options.sockets.handleFanoutInboundResult(
          connectionId,
          message,
          subscriber
        ),
        this.options.inboundDispatchBudget,
        this.receiveRoundRobin
      );
      state.openingToken = undefined;
      state.active = { token, loop };
      this.subscriberReceiveLoops.push(loop);
      return taskRunner.run(
        `subscriber:${channelName}:manual:${state.index}`,
        signal => loop.run(signal)
      );
    } catch (error) {
      state.openingToken = undefined;
      return Promise.allSettled([
        this.options.sockets.closeFanoutSubscriberConnection(connectionId)
      ]).then(results => {
        const closeError = results[0].status === 'rejected' ? results[0].reason : undefined;
        if (closeError === undefined) throw error;
        throw new AggregateError(
          [error, closeError],
          `Fanout subscriber '${connectionId}' failed to open and close.`
        );
      });
    }
  }

  private async closeManualFanoutSubscriber(
    state: ManualFanoutSubscriberState,
    expectedToken?: symbol
  ): Promise<void> {
    const active = state.active;
    if (active === undefined || (expectedToken !== undefined && active.token !== expectedToken)) return;
    state.active = undefined;
    state.stopping = active;
    const errors: unknown[] = [];
    try {
      await active.loop.stop();
    } catch (error) {
      errors.push(error);
    } finally {
      this.removeSubscriberReceiveLoop(active.loop);
      state.stopping = undefined;
    }
    try {
      await this.options.sockets.closeFanoutSubscriberConnection(state.connectionId);
    } catch (error) {
      errors.push(error);
    }
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `Fanout subscriber '${state.connectionId}' cleanup failed.`);
    }
  }

  private shouldReconnectManualFanoutSubscriber(state: ManualFanoutSubscriberState): boolean {
    return !this.disposed && state.desired;
  }

  private drainManualFanoutSubscriber(state: ManualFanoutSubscriberState): Promise<void> {
    state.transition = state.transition.then(
      () => this.closeManualFanoutSubscriber(state),
      () => this.closeManualFanoutSubscriber(state)
    );
    return state.transition;
  }

  private startFanoutSubscriberLoop(
    channelName: string,
    connectionId: string,
    subscriber: import('../backend/contracts').ZLinkBackendSubscriberSocket
  ): () => Promise<void> {
    const taskRunner = this.taskRunner;
    const channel = this.options.registration.channels.get(channelName);
    if (taskRunner === undefined || channel === undefined) {
      throw new ZLinkConfigurationException(
        `Fanout subscriber '${channelName}' requires a running task runner.`
      );
    }
    const dispatcher = new ZLinkChannelPublishDispatcher({
      channelName,
      codecs: this.options.codecs,
      dispatchErrors: this.options.dispatchServices
        .dispatchErrorReporter(taskRunner.errorSink),
      handlers: new Map(channel.publishHandlers?.map(handler => [
        handler.packetName,
        handler.handler
      ])),
      filters: this.options.dispatchServices.handlerFilters(),
      unhandled: this.options.registration.dispatch?.unhandled,
      metrics: this.options.dispatchServices.metrics()
    });
    const loop = new ZLinkSubscriberReceiveLoop(
      this.options.adapter,
      subscriber,
      dispatcher,
      message => this.options.sockets.handleFanoutInboundResult(
        connectionId,
        message,
        subscriber
      ),
      this.options.inboundDispatchBudget
    );
    this.subscriberReceiveLoops.push(loop);
    void taskRunner.run(
      `subscriber:${channelName}:automatic:${connectionId}`,
      signal => loop.run(signal)
    );
    return async () => {
      await loop.stop();
      this.removeSubscriberReceiveLoop(loop);
    };
  }

  private removeSubscriberReceiveLoop(loop: ZLinkSubscriberReceiveLoop): void {
    const index = this.subscriberReceiveLoops.indexOf(loop);
    if (index >= 0) this.subscriberReceiveLoops.splice(index, 1);
  }

  private startRouteReceivers(taskRunner: ZLinkRuntimeTaskRunner | undefined): Promise<void>[] {
    const tasks: Promise<void>[] = [];
    for (const routeChannel of this.options.registration.routeChannelOptions.values()) {
      const spotRouteNode = this.options.spotRoutes.spotRouteNode(routeChannel.routerChannelId);
      if (routeChannel.bind === undefined && spotRouteNode === undefined) {
        continue;
      }
      const router = this.options.sockets.routeRouter(routeChannel.routerChannelId);
      const spotRouteBridge = this.createSpotRouteBridgeForRouter(routeChannel.routerChannelId, router);
      const handlers = [
        ...collectRouteChannelHandlers(routeChannel),
        ...[...this.options.internalRouteSendHandlers?.entries() ?? []].map(
          ([packetName, handler]): ZLinkRouteHandlerRegistration => ({ kind: 'send', packetName, handler })
        ),
        ...[...this.options.internalRouteRequestHandlers?.entries() ?? []].map(
          ([packetName, handler]): ZLinkRouteHandlerRegistration => ({ kind: 'request', packetName, handler })
        )
      ];
      if (taskRunner === undefined) {
        throw new ZLinkConfigurationException(
          `Route channel '${routeChannel.routerChannelId}' dispatch requires a runtime task runner.`
        );
      }
      const dispatcher = new ZLinkRoutePacketDispatcher({
        routerChannelId: routeChannel.routerChannelId,
        codecs: this.options.codecs,
        dispatchErrors: this.options.dispatchServices.dispatchErrorReporter(taskRunner.errorSink),
        handlers,
        unhandled: this.options.registration.dispatch?.unhandled,
        replySubmitter: this.options.sockets.requireSubmitter(router),
        spotRouteBridge,
        rawBridgeReplyHandler: (received) =>
          this.options.spotRouteBridgeRawReplies.tryComplete(routeChannel.routerChannelId, received)
      });
      const loop = new ZLinkRouteReceiveLoop(
        router,
        dispatcher,
        this.options.inboundDispatchBudget,
        this.options.adapter.createReadablePoller(router),
        this.receiveRoundRobin
      );
      this.routeReceiveLoops.push(loop);
      tasks.push(taskRunner.run(`route:${routeChannel.routerChannelId}`, (signal) => loop.run(signal)));
    }
    return tasks;
  }

  private createSpotRouteBridgeForRouter(
    channelName: string,
    router: ZLinkBackendRouterSocket
  ): ZLinkBackendSpotRouteBridge | undefined {
    const spotRouteNode = this.options.spotRoutes.spotRouteNode(channelName);
    if (spotRouteNode === undefined) {
      return undefined;
    }
    const bridge = spotRouteNode.createRouteBridge();
    bridge.attachRouterChannel(channelName, router, {
      capabilities: ZLINK_BACKEND_SPOT_ROUTE_BRIDGE_ROUTE_WITH_CHANNEL_INBOUND
    });
    this.options.spotRouteBridges.set(channelName, bridge);
    return bridge;
  }
}

function adaptRouteRequestHandler(
  handler: ZLinkRouteRuntimeRequestHandler
): ZLinkRequestHandler<unknown, unknown> {
  return {
    handle: async (payload, context) => handler.handle(payload, context as never)
  };
}

function adaptRouteSendHandler(
  handler: ZLinkRouteRuntimeSendHandler
): ZLinkSendHandler<unknown> {
  return {
    handle: async (payload, context) => {
      await handler.handle(payload, context as never);
    }
  };
}

function requireChannelHandlerType(
  handlerType: import('../../contracts').Type | undefined,
  channelName: string,
  packetName: string
): import('../../contracts').Type {
  if (handlerType === undefined) {
    throw new ZLinkConfigurationException(
      `Channel '${channelName}' handler '${packetName}' has neither an instance nor a handler type.`
    );
  }
  return handlerType;
}

function meshChannelKey(meshName: string, channelName: string): string {
  return `${meshName}\u0000${channelName}`;
}

function manualFanoutSubscriberKey(channelName: string, endpoint: string): string {
  return `manual\0${channelName}\0${endpoint}`;
}

function hasClientServerLocationTopology(registration: ZLinkFrameworkRegistration): boolean {
  for (const channel of registration.channels.values()) {
    if (channel.server !== undefined
      || (channel.client !== undefined && (channel.client.manualConnections?.length ?? 0) === 0)) {
      return true;
    }
  }
  return false;
}

function hasFanoutLocationTopology(
  registration: ZLinkFrameworkRegistration
): boolean {
  for (const channel of registration.channels.values()) {
    if (channel.publisher !== undefined
      || (channel.subscriber !== undefined
        && (channel.subscriber.manualConnections?.length ?? 0) === 0)) {
      return true;
    }
  }
  return false;
}
