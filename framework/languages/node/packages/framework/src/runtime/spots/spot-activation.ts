import type {
  ActorRef,
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkChannelClient,
  ZLinkFanoutClient,
  ZLinkMessageSerializer,
  ZLinkInstanceSpot,
  ZLinkSpot,
  ZLinkSpotCreateResponse,
  ZLinkSpotPublisherClient,
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeAdmissionGate } from '../admission';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import type {
  ZLinkSpotActorRequestHandlerRegistration,
  ZLinkSpotActorSendHandlerRegistration,
  ZLinkSpotPacketHandlerRegistration,
  ZLinkSpotSubscriptionHandlerRegistration,
  ZLinkSpotTimerHandlerRegistration
} from '../../contracts/Configuration/RegistrationTypes';
import {
  ZLinkSpotCloseReason,
  ZLinkSpotCreateState,
  ZLinkSpotRelocationReadinessMode,
  ZLinkUserSpotExecutionMode
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import { throwIfAborted } from '../abort';
import { ZLinkConfigurationException } from '../configuration';
import type {
  ZLinkBackendReceived,
  ZLinkBackendSpot,
  ZLinkBackendSpotNode,
  ZLinkBackendTopicMessage
} from '../backend/contracts';
import { ZLinkDispatchErrorReporter } from '../channels';
import {
  ZLinkSpotActorHandlerRegistryRuntime,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import { ZLinkWorkerRuntime } from '../workers';
import { disposeLifecycleHandlers } from '../handlers/handler-instance-scope';
import {
  decodeFrameworkPayloadMessage,
  encodeFrameworkPayloadMessage,
  wrapFrameworkPayloadMessage
} from '../messaging/payload-codec';
import { createFreshProviderInstance } from './spot-provider';
import {
  DefaultZLinkSpotOutbound,
  type ZLinkSpotAddressTransport,
  type ZLinkSpotRoutedTransport
} from './spot-outbound';
import {
  applySpotHandlerRegistrations,
  DefaultZLinkInstanceSpotHandlerRegistry,
  DefaultZLinkSpotHandlerRegistry
} from './spot-handler-registry';
import {
  addSpotTimerRegistrations,
  ZLinkSpotTimerRegistry
} from './spot-timer';
import { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import { createInstanceSpotContext, createSpotContext } from './spot-context';
import type { ZLinkSpotActorJoinDispatch, ZLinkDetachedTaskRunner } from './spot-actor-join-dispatch';
import { ZLinkSpotActorAdmissionCoordinator } from './spot-actor-admission-coordinator';
import { ZLinkSpotActivation, ZLinkSpotCloseOccupiedError } from './spot-activation-state';
import type { ZLinkSpotLocationClaim } from './spot-location-claim';
import type {
  ZLinkSpotActorHandoffRuntime,
  ZLinkSpotActorTransferRuntime,
  ZLinkSpotBoundSessionRuntime
} from './spot-runtime-ports';
import type { ZLinkLocalSpotCreateResult } from './spot-manager-internal-contracts';
import { invokeSpotClosing } from './spot-closing';

export interface ZLinkSpotActivationLifecycleOptions {
  readonly spotTimerHandlers?: readonly ZLinkSpotTimerHandlerRegistration[];
  readonly spotPacketHandlers?: readonly ZLinkSpotPacketHandlerRegistration[];
  readonly spotSubscriptionHandlers?: readonly ZLinkSpotSubscriptionHandlerRegistration[];
  readonly spotActorSendHandlers?: readonly ZLinkSpotActorSendHandlerRegistration[];
  readonly spotActorRequestHandlers?: readonly ZLinkSpotActorRequestHandlerRegistration[];
  readonly nodeRid?: RoutingId;
  readonly nodeRidProvider?: (meshName: string) => RoutingId | undefined;
  readonly actorCountProvider?: (spotId: RoutingId) => number;
  readonly userSpotExecutionMode?: (
    meshName: string,
    spotType: Type<ZLinkSpot>
  ) => ZLinkUserSpotExecutionMode;
  readonly userSpotRelocationReadiness?: (
    meshName: string,
    spotType: Type<ZLinkSpot>
  ) => ZLinkSpotRelocationReadinessMode;
  readonly channelClient?: ZLinkChannelClient;
  readonly fanoutClient?: ZLinkFanoutClient;
  readonly spotPublisherClient?: ZLinkSpotPublisherClient;
  readonly routedTransport?: ZLinkSpotRoutedTransport;
  readonly addressTransport?: ZLinkSpotAddressTransport;
  readonly spotRouterChannelIdForMesh?: (meshName: string) => string;
  readonly channelMeshNameForChannel?: (channelName: string) => string | undefined;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly workerRuntime: ZLinkWorkerRuntime;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly locationClaim: ZLinkSpotLocationClaim;
  readonly createNativeSpot?: (
    meshName: string,
    spotId: RoutingId,
    authority?: ZLinkNativeSpotAuthority
  ) => ZLinkBackendSpot | undefined;
  readonly createReceived?: () => ZLinkBackendReceived;
  readonly createTopicMessage?: () => ZLinkBackendTopicMessage;
  readonly nativeSpotNodeProvider?: (meshName: string) => ZLinkBackendSpotNode | undefined;
  readonly actorResolver?: (actorId: string) => ZLinkActor | undefined;
  readonly actorTransferRuntime?: ZLinkSpotActorTransferRuntime;
  readonly boundSessionRuntime?: ZLinkSpotBoundSessionRuntime;
  readonly actorHandoffRuntime?: ZLinkSpotActorHandoffRuntime;
  readonly detachedTaskRunner?: ZLinkDetachedTaskRunner;
  readonly admission?: ZLinkRuntimeAdmissionGate;
  readonly leaveActor: (
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal,
    meshName?: string
  ) => Promise<void>;
  readonly closeSpot: (
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal,
    reason?: ZLinkSpotCloseReason
  ) => Promise<boolean>;
  readonly registerActivation: (activation: ZLinkSpotActivation) => void;
  readonly releaseLocation: (
    activation: ZLinkSpotActivation,
    meshName: string,
    spotId: RoutingId
  ) => Promise<void>;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
}

export interface ZLinkNativeSpotAuthority {
  readonly objectKind?: 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
}

interface UserSpotLocationClaim {
  readonly meshName: string;
}

export class ZLinkSpotActivationLifecycle {
  private readonly actorAdmission: ZLinkSpotActorAdmissionCoordinator;
  private readonly cleanupStates = new WeakMap<ZLinkSpotActivation, {
    closingAttempted: boolean;
    timersDisposed: boolean;
    handlersDisposed: boolean;
    nativeDisposed: boolean;
    locationReleased: boolean;
    inFlight?: Promise<void>;
  }>();

  constructor(private readonly options: ZLinkSpotActivationLifecycleOptions) {
    this.actorAdmission = new ZLinkSpotActorAdmissionCoordinator(options);
  }

  async materializeRelocation<TSpot extends ZLinkSpot | ZLinkInstanceSpot>(
    meshName: string,
    objectKind: 'user_spot' | 'instance_spot',
    stableType: string,
    implementation: Type<TSpot>,
    spotId: RoutingId,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkSpotActivation> {
    const serial = new ZLinkSpotSerialExecutor(true, spotId);
    const actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
    const handlers = new DefaultZLinkSpotHandlerRegistry(actorHandlers);
    applySpotHandlerRegistrations(
      handlers,
      implementation as unknown as Type<ZLinkSpot>,
      objectKind === 'user_spot'
        ? {
            actorSendHandlers: this.options.spotActorSendHandlers,
            actorRequestHandlers: this.options.spotActorRequestHandlers,
            packetHandlers: this.options.spotPacketHandlers,
            subscriptionHandlers: this.options.spotSubscriptionHandlers
          }
        : { packetHandlers: this.options.spotPacketHandlers }
    );
    const timers = new ZLinkSpotTimerRegistry(
      this.options.metrics,
      () => this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
    );
    const outbound = new DefaultZLinkSpotOutbound(
      serial,
      this.options.channelClient,
      this.options.fanoutClient,
      this.options.spotPublisherClient,
      this.options.routedTransport,
      this.options.spotRouterChannelIdForMesh ?? ((selectedMesh) => selectedMesh),
      undefined,
      meshName,
      undefined,
      this.options.addressTransport
    );
    const nativeSpot = this.options.createNativeSpot?.(meshName, spotId, {
      objectKind,
      stableType,
      objectGeneration,
      authorityOwnerGeneration
    });
    let instance: TSpot | undefined;
    let activation: ZLinkSpotActivation | undefined;
    const common = {
      meshName,
      spotId,
      objectGeneration: toContextGeneration(objectGeneration),
      outbound,
      timers,
      serial,
      getSpot: () => instance as unknown as ZLinkSpot,
      nodeRid: this.options.nodeRid,
      nodeRidProvider: () => this.options.nodeRidProvider?.(meshName),
      providerResolver: this.options.providerResolver,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      workerRuntime: this.options.workerRuntime,
      close: (contextSignal?: AbortSignal) =>
        this.options.closeSpot(meshName, spotId, contextSignal)
    };
    const context = objectKind === 'user_spot'
      ? createSpotContext({
          ...common,
          handlers,
          relocationReady: () => {
            if (activation === undefined) {
              throw new ZLinkConfigurationException(
                'Spot relocation readiness is unavailable before activation.'
              );
            }
            return activation.relocationReadyCall();
          },
          ensureOperationAllowed: () => activation?.ensureContextOperationAllowed(),
          leaveActor: (actor, contextSignal) =>
            this.options.leaveActor(spotId, actor, contextSignal, meshName)
        })
      : createInstanceSpotContext({
          ...common,
          handlers: new DefaultZLinkInstanceSpotHandlerRegistry(handlers)
        });
    try {
      instance = await createFreshProviderInstance(
        implementation,
        this.options.providerResolver,
        context
      );
      Object.defineProperty(instance, 'context', {
        configurable: true,
        enumerable: false,
        value: context
      });
      await instance.configure?.();
      await addSpotTimerRegistrations(
        timers,
        implementation as unknown as Type<ZLinkSpot>,
        spotId,
        instance as unknown as ZLinkSpot,
        serial,
        { timerHandlers: this.options.spotTimerHandlers },
        {
          providerResolver: this.options.providerResolver,
          runtimeEventPublisher: this.options.runtimeEventPublisher,
          signal
        }
      );
      activation = new ZLinkSpotActivation({
        meshName,
        spotId,
        objectGeneration,
        spotType: implementation as unknown as Type<ZLinkSpot>,
        spot: instance as unknown as ZLinkSpot,
        serial,
        relocationReadiness: objectKind === 'user_spot'
          ? this.options.userSpotRelocationReadiness?.(
              meshName,
              implementation as unknown as Type<ZLinkSpot>
            )
          : ZLinkSpotRelocationReadinessMode.AnyTurnBoundary,
        timers,
        actorHandlers,
        handlers,
        externalActorCount: () => this.options.actorCountProvider?.(spotId) ?? 0,
        nativeSpot,
        closeWhenReady: (reason) => this.scheduleDrainClose(meshName, spotId, reason),
        metrics: this.options.metrics
      });
      this.options.registerActivation(activation);
      return activation;
    } catch (error) {
      await timers.dispose().catch(() => undefined);
      if (instance !== undefined) {
        await disposeLifecycleHandlers(instance).catch(() => undefined);
      }
      await nativeSpot?.dispose().catch(() => undefined);
      throw error;
    }
  }

  async materializeInstance<TSpot extends ZLinkInstanceSpot>(
    meshName: string,
    instanceType: string,
    implementation: Type<TSpot>,
    spotId: RoutingId,
    objectGeneration: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkSpotActivation> {
    const serial = new ZLinkSpotSerialExecutor(true, spotId);
    const actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
    const handlers = new DefaultZLinkSpotHandlerRegistry(actorHandlers);
    const instanceHandlers = new DefaultZLinkInstanceSpotHandlerRegistry(handlers);
    applySpotHandlerRegistrations(handlers, implementation as unknown as Type<ZLinkSpot>, {
      packetHandlers: this.options.spotPacketHandlers
    });
    const timers = new ZLinkSpotTimerRegistry(
      this.options.metrics,
      () => this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
    );
    const outbound = new DefaultZLinkSpotOutbound(
      serial,
      this.options.channelClient,
      this.options.fanoutClient,
      this.options.spotPublisherClient,
      this.options.routedTransport,
      this.options.spotRouterChannelIdForMesh ?? ((selectedMesh) => selectedMesh),
      undefined,
      meshName,
      undefined,
      this.options.addressTransport
    );
    let instance: TSpot | undefined;
    const context = createInstanceSpotContext({
        meshName,
        spotId,
        objectGeneration: toContextGeneration(objectGeneration),
        handlers: instanceHandlers,
        outbound,
        timers,
        serial,
        getSpot: () => instance as unknown as ZLinkSpot,
        nodeRid: this.options.nodeRid,
        nodeRidProvider: () => this.options.nodeRidProvider?.(meshName),
        providerResolver: this.options.providerResolver,
        runtimeEventPublisher: this.options.runtimeEventPublisher,
        workerRuntime: this.options.workerRuntime,
        close: (contextSignal) => this.options.closeSpot(meshName, spotId, contextSignal)
      });
    instance = await createFreshProviderInstance(
      implementation,
      this.options.providerResolver,
      context
    );
    Object.defineProperty(instance, 'context', {
      configurable: true,
      enumerable: false,
      value: context
    });
    const activation = new ZLinkSpotActivation({
      meshName,
      spotId,
      objectGeneration,
      spotType: implementation as unknown as Type<ZLinkSpot>,
      spot: instance as unknown as ZLinkSpot,
      serial,
      timers,
      actorHandlers,
      handlers,
      externalActorCount: () => 0,
      closeWhenReady: (reason) => this.scheduleDrainClose(meshName, spotId, reason)
    });
    try {
      await instance.configure?.();
      await addSpotTimerRegistrations(
        timers,
        implementation as unknown as Type<ZLinkSpot>,
        spotId,
        instance as unknown as ZLinkSpot,
        serial,
        { timerHandlers: this.options.spotTimerHandlers },
        {
          providerResolver: this.options.providerResolver,
          runtimeEventPublisher: this.options.runtimeEventPublisher,
          signal
        }
      );
      await serial.execute(() => instance!.onInitialize?.());
      this.options.registerActivation(activation);
      return activation;
    } catch (error) {
      await timers.dispose();
      await disposeLifecycleHandlers(instance);
      throw new AggregateError(
        [error],
        `Instance Spot '${instanceType}' materialization failed for '${String(spotId)}'.`
      );
    }
  }

  async discardInstance(activation: ZLinkSpotActivation): Promise<void> {
    const errors: unknown[] = [];
    try {
      await activation.serial.execute(() => invokeSpotClosing(
        activation.spot.onClosing?.bind(activation.spot),
        ZLinkSpotCloseReason.ExplicitClose
      ));
    } catch (error) {
      errors.push(error);
    }
    try {
      await activation.timers.dispose();
    } catch (error) {
      errors.push(error);
    }
    try {
      await disposeLifecycleHandlers(activation.spot);
    } catch (error) {
      errors.push(error);
    }
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(
        errors,
        `Instance Spot '${String(activation.spotId)}' cleanup failed.`
      );
    }
  }

  resourcesReleased(activation: ZLinkSpotActivation): boolean {
    const state = this.cleanupStates.get(activation);
    return state?.timersDisposed === true &&
      state.handlersDisposed === true &&
      state.nativeDisposed === true &&
      state.locationReleased === true;
  }

  async create<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    request: Message,
    signal?: AbortSignal,
    authority?: ZLinkNativeSpotAuthority
  ): Promise<ZLinkLocalSpotCreateResult> {
    const executionMode = this.options.userSpotExecutionMode?.(meshName, spotType)
      ?? ZLinkUserSpotExecutionMode.SpotWide;
    const serial = new ZLinkSpotSerialExecutor(
      executionMode === ZLinkUserSpotExecutionMode.SpotWide,
      spotId
    );
    const timerSerials = new Map<string, ZLinkSpotSerialExecutor>();
    const actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
    const handlers = new DefaultZLinkSpotHandlerRegistry(actorHandlers);
    applySpotHandlerRegistrations(handlers, spotType, {
      actorSendHandlers: this.options.spotActorSendHandlers,
      actorRequestHandlers: this.options.spotActorRequestHandlers,
      packetHandlers: this.options.spotPacketHandlers,
      subscriptionHandlers: this.options.spotSubscriptionHandlers
    });
    const timers = new ZLinkSpotTimerRegistry(
      this.options.metrics,
      () => this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true,
      (name, fallback) => {
        if (executionMode === ZLinkUserSpotExecutionMode.SpotWide) {
          return fallback;
        }
        let timerSerial = timerSerials.get(name);
        if (timerSerial === undefined) {
          timerSerial = new ZLinkSpotSerialExecutor(false);
          timerSerials.set(name, timerSerial);
        }
        return timerSerial;
      }
    );
    let nativeSpot: ZLinkBackendSpot | undefined;
    const outbound = new DefaultZLinkSpotOutbound(
      serial,
      this.options.channelClient,
      this.options.fanoutClient,
      this.options.spotPublisherClient,
      this.options.routedTransport,
      this.options.spotRouterChannelIdForMesh ?? ((meshName) => meshName),
      () => nativeSpot,
      meshName,
      this.options.channelMeshNameForChannel,
      this.options.addressTransport
    );
    // Core owns the lifecycle generation. Publish the location only after the
    // formal Spot exists, so no synthetic generation can escape into routing.
    nativeSpot = this.options.createNativeSpot?.(meshName, spotId, authority);
    const spotGeneration = nativeSpot?.lifecycleGeneration;
    if (nativeSpot === undefined || spotGeneration === undefined || spotGeneration <= 0n) {
      await nativeSpot?.dispose();
      if (!this.options.locationClaim.enabled) {
        const unclaimed = { claimed: true as const, meshName: '' };
        return await this.createWithoutNativeSpot(
          meshName,
          spotType,
          spotId,
          request,
          serial,
          actorHandlers,
          handlers,
          timers,
          outbound,
          unclaimed,
          executionMode,
          authority?.objectGeneration ?? 0n,
          signal
        );
      }
      throw new ZLinkConfigurationException('Core Spot lifecycle generation is not available.');
    }
    const locationClaim = await this.options.locationClaim.claimUserSpot(
      meshName,
      spotId,
      spotType.name,
      spotGeneration
    );
    if (!locationClaim.claimed) {
      await nativeSpot.dispose();
      return { spotId, state: ZLinkSpotCreateState.Existing };
    }

    let spot: ZLinkSpot | undefined;
    let activation: ZLinkSpotActivation | undefined;
    let lifecycleStarted = false;
    const context = createSpotContext({
      meshName,
      spotId,
      objectGeneration: toContextGeneration(spotGeneration),
      handlers,
      outbound,
      timers,
      serial,
      getSpot: () => spot,
      nodeRid: this.options.nodeRid,
      nodeRidProvider: () => this.options.nodeRidProvider?.(meshName),
      providerResolver: this.options.providerResolver,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      workerRuntime: this.options.workerRuntime,
      relocationReady: () => {
        if (activation === undefined) {
          throw new ZLinkConfigurationException(
            'Spot relocation readiness is unavailable before activation.'
          );
        }
        return activation.relocationReadyCall();
      },
      ensureOperationAllowed: () => activation?.ensureContextOperationAllowed(),
      leaveActor: (actor, contextSignal) =>
        this.options.leaveActor(spotId, actor, contextSignal, meshName),
      close: async (contextSignal) => {
        // Native callbacks can cross a promise boundary that does not retain
        // the serial turn context. Queue the close before calling the manager
        // so the callback never waits for work behind its own serial turn.
        if (activation?.serial.isExecuting === true && !activation.serial.isCurrentTurn) {
          activation.requestClose();
          const retry = activation.serial.post(() => this.options.closeSpot(meshName, spotId, contextSignal));
          this.options.detachedTaskRunner?.runDetached(`spot close ${String(spotId)}`, async () => { await retry; });
          if (this.options.detachedTaskRunner === undefined) void retry.catch(() => undefined);
          return true;
        }
        return await this.options.closeSpot(meshName, spotId, contextSignal);
      }
    });
    try {
      spot = await createFreshProviderInstance(spotType, this.options.providerResolver, context);
      Object.defineProperty(spot, 'context', {
        configurable: true,
        enumerable: false,
        value: context
      });

      // getOrCreateSpot registers the native Spot under this rid so core routes
      // actor-join admission requests to it (createSpot alone does not register).
      activation = new ZLinkSpotActivation({
        meshName,
        spotId,
        spotType,
        spot,
        serial,
        executionMode,
        relocationReadiness: this.options.userSpotRelocationReadiness?.(meshName, spotType),
        timers,
        actorHandlers,
        handlers,
        externalActorCount: () => this.options.actorCountProvider?.(spotId) ?? 0,
        nativeSpot,
        closeWhenReady: (reason) => this.scheduleDrainClose(meshName, spotId, reason),
        metrics: this.options.metrics
      });
      const nativeDispatch = this.actorAdmission.attachNativeActorJoinDispatch(activation, nativeSpot);
      activation.actorDispatch = nativeDispatch;
      lifecycleStarted = true;
      return await this.runCreateLifecycle(activation, spotType, request, locationClaim, nativeDispatch, signal);
    } catch (error) {
      const cleanupErrors: unknown[] = [];
      try {
        if (activation !== undefined) {
          await this.cleanupActivation(activation, locationClaim.meshName, lifecycleStarted, signal);
        } else {
          const partialCleanup = await Promise.allSettled([
            timers.dispose(),
            ...(spot === undefined ? [] : [disposeLifecycleHandlers(spot)]),
            nativeSpot.dispose(),
            this.options.locationClaim.release(locationClaim.meshName, spotId)
          ]);
          const partialErrors = partialCleanup
            .filter((result): result is PromiseRejectedResult => result.status === 'rejected')
            .map((result) => result.reason);
          if (partialErrors.length === 1) throw partialErrors[0];
          if (partialErrors.length > 1) {
            throw new AggregateError(partialErrors, `Spot '${spotId}' partial creation cleanup failed.`);
          }
        }
      } catch (cleanupError) {
        cleanupErrors.push(cleanupError);
      }
      if (cleanupErrors.length > 0) {
        throw new AggregateError([error, ...cleanupErrors], `Spot '${spotId}' creation cleanup failed.`);
      }
      throw error;
    }
  }

  private async createWithoutNativeSpot<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    request: Message,
    serial: ZLinkSpotSerialExecutor,
    actorHandlers: ZLinkSpotActorHandlerRegistryRuntime,
    handlers: DefaultZLinkSpotHandlerRegistry,
    timers: ZLinkSpotTimerRegistry,
    outbound: DefaultZLinkSpotOutbound,
    locationClaim: UserSpotLocationClaim,
    executionMode: ZLinkUserSpotExecutionMode,
    objectGeneration: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult> {
    let spot: ZLinkSpot | undefined;
    let activation: ZLinkSpotActivation | undefined;
    const context = createSpotContext({
      meshName,
      spotId,
      objectGeneration: toContextGeneration(objectGeneration),
      handlers,
      outbound,
      timers,
      serial,
      getSpot: () => spot,
      nodeRid: this.options.nodeRid,
      nodeRidProvider: () => this.options.nodeRidProvider?.(meshName),
      providerResolver: this.options.providerResolver,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      workerRuntime: this.options.workerRuntime,
      relocationReady: () => {
        if (activation === undefined) {
          throw new ZLinkConfigurationException(
            'Spot relocation readiness is unavailable before activation.'
          );
        }
        return activation.relocationReadyCall();
      },
      ensureOperationAllowed: () => activation?.ensureContextOperationAllowed(),
      leaveActor: (actor, contextSignal) =>
        this.options.leaveActor(spotId, actor, contextSignal, meshName),
      close: (contextSignal) => this.options.closeSpot(meshName, spotId, contextSignal)
    });
    spot = await createFreshProviderInstance(spotType, this.options.providerResolver, context);
    Object.defineProperty(spot, 'context', { configurable: true, enumerable: false, value: context });
    activation = new ZLinkSpotActivation({
      meshName,
      spotId,
      spotType,
      spot,
      serial,
      executionMode,
      relocationReadiness: this.options.userSpotRelocationReadiness?.(meshName, spotType),
      timers,
      actorHandlers,
      handlers,
      externalActorCount: () => this.options.actorCountProvider?.(spotId) ?? 0,
      closeWhenReady: (reason) => this.scheduleDrainClose(meshName, spotId, reason),
      metrics: this.options.metrics
    });
    return await this.runCreateLifecycle(activation, spotType, request, locationClaim, undefined, signal);
  }

  private scheduleDrainClose(
    meshName: string,
    spotId: RoutingId,
    reason: ZLinkSpotCloseReason
  ): void {
    const close = async () => {
      await this.options.closeSpot(meshName, spotId, undefined, reason);
    };
    this.options.detachedTaskRunner?.runDetached(`spot drain close ${String(spotId)}`, close);
    if (this.options.detachedTaskRunner === undefined) {
      void close().catch(() => undefined);
    }
  }

  async close(
    activation: ZLinkSpotActivation,
    signal?: AbortSignal,
    reason = ZLinkSpotCloseReason.ExplicitClose
  ): Promise<void> {
    const seal = activation.sealExecution();
    await this.closeAfterSeal(activation, seal, signal, reason);
  }

  async closeAfterSeal(
    activation: ZLinkSpotActivation,
    seal: import('../execution').ZLinkExecutionBarrierSeal,
    signal?: AbortSignal,
    reason = ZLinkSpotCloseReason.ExplicitClose
  ): Promise<void> {
    try {
      await activation.waitForExecutionQuiescence(seal, signal);
    } catch (error) {
      activation.abortExecutionSeal(seal);
      throw error;
    }
    // The eager occupancy check that authorized this seal (startClose ->
    // canClose()) ran before quiescence, so it can miss an actor join that
    // was already queued on the serial executor at that moment. Recheck now
    // that every turn admitted before the seal has finished, and release the
    // seal instead of closing an occupied Spot.
    if (!activation.canClose()) {
      activation.abortExecutionSeal(seal);
      throw new ZLinkSpotCloseOccupiedError(activation.spotId);
    }
    if (!activation.commitExecutionSeal(seal)) {
      throw new Error(`Spot '${String(activation.spotId)}' close seal is stale.`);
    }
    await this.cleanupActivation(
      activation,
      activation.meshName,
      true,
      signal,
      reason
    );
  }

  async dispatchActorPacket(
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    return await this.actorAdmission.dispatchActorPacket(
      activation,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal,
      messageFollowOrigin
    );
  }

  private async runCreateLifecycle<TSpot extends ZLinkSpot>(
    activation: ZLinkSpotActivation,
    spotType: Type<TSpot>,
    request: Message,
    locationClaim: UserSpotLocationClaim,
    nativeDispatch: ZLinkSpotActorJoinDispatch | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult> {
    try {
      await activation.spot.configure?.();
      nativeDispatch?.configureSubscriptions(activation.handlers.snapshot());
      await addSpotTimerRegistrations(
        activation.timers,
        spotType,
        activation.spotId,
        activation.spot,
        activation.serial,
        { timerHandlers: this.options.spotTimerHandlers },
        {
          providerResolver: this.options.providerResolver,
          runtimeEventPublisher: this.options.runtimeEventPublisher,
          signal
        }
      );
      let createResponse: ZLinkSpotCreateResponse | undefined;
      await activation.serial.execute(async () => {
        createResponse = await activation.spot.onCreate?.(
          wrapFrameworkPayloadMessage(request, this.options.messageSerializers)
        );
        if (createResponse?.accepted === false) {
          return;
        }
        await activation.spot.onInitialize?.();
      });
      if (createResponse?.accepted === false) {
        await this.cleanupActivation(activation, locationClaim.meshName, false, signal);
        return {
          spotId: activation.spotId,
          state: ZLinkSpotCreateState.Rejected,
          reply: this.decodeCreateReply(createResponse.reply)
        };
      }
      this.options.registerActivation(activation);
      return {
        spotId: activation.spotId,
        state: ZLinkSpotCreateState.Created,
        reply: this.decodeCreateReply(createResponse?.reply)
      };
    } catch (error) {
      try {
        await this.cleanupActivation(activation, locationClaim.meshName, true, signal);
      } catch (cleanupError) {
        throw new AggregateError([error, cleanupError], `Spot '${activation.spotId}' creation cleanup failed.`);
      }
      throw error;
    }
  }

  private async cleanupActivation(
    activation: ZLinkSpotActivation,
    locationMeshName: string,
    notifyClosing: boolean,
    signal?: AbortSignal,
    reason = ZLinkSpotCloseReason.ExplicitClose
  ): Promise<void> {
    throwIfAborted(signal);
    const state = this.cleanupStates.get(activation) ?? {
      closingAttempted: false,
      timersDisposed: false,
      handlersDisposed: false,
      nativeDisposed: false,
      locationReleased: false
    };
    this.cleanupStates.set(activation, state);
    if (state.inFlight !== undefined) return await state.inFlight;
    state.inFlight = this.runCleanup(activation, locationMeshName, notifyClosing, reason, state)
      .finally(() => { state.inFlight = undefined; });
    return await state.inFlight;
  }

  private async runCleanup(
    activation: ZLinkSpotActivation,
    locationMeshName: string,
    notifyClosing: boolean,
    reason: ZLinkSpotCloseReason,
    state: {
      closingAttempted: boolean;
      timersDisposed: boolean;
      handlersDisposed: boolean;
      nativeDisposed: boolean;
      locationReleased: boolean;
    }
  ): Promise<void> {
    const errors: unknown[] = [];
    const cleanup = async (operation: () => Promise<void> | void, completed: () => void) => {
      try {
        await operation();
        completed();
      } catch (error) {
        errors.push(error);
      }
    };
    if (notifyClosing && !state.closingAttempted) {
      state.closingAttempted = true;
      await cleanup(() => invokeSpotClosing(
        activation.spot.onClosing?.bind(activation.spot),
        reason
      ), () => undefined);
    }
    if (!state.timersDisposed) {
      await cleanup(() => activation.timers.dispose(), () => { state.timersDisposed = true; });
    }
    if (!state.handlersDisposed) {
      await cleanup(
        () => disposeLifecycleHandlers(activation.spot),
        () => { state.handlersDisposed = true; }
      );
    }
    if (!state.nativeDisposed) {
      await cleanup(() => activation.actorDispatch?.dispose(), () => undefined);
      await cleanup(() => activation.nativeSpot?.dispose(), () => { state.nativeDisposed = true; });
    }
    if (!state.locationReleased) {
      await cleanup(
        () => this.options.releaseLocation(activation, locationMeshName, activation.spotId),
        () => { state.locationReleased = true; }
      );
    }
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) {
      throw new AggregateError(errors, `Spot '${activation.spotId}' cleanup failed.`);
    }
  }

  private decodeCreateReply(reply: unknown): unknown {
    if (reply === undefined) {
      return undefined;
    }
    const message = encodeFrameworkPayloadMessage(reply, this.options.messageSerializers);
    try {
      return decodeFrameworkPayloadMessage(message, this.options.messageSerializers);
    } finally {
      message.close();
    }
  }
}

function toContextGeneration(generation: bigint): number {
  if (generation < 0n || generation > BigInt(Number.MAX_SAFE_INTEGER)) {
    throw new ZLinkConfigurationException(
      `Spot object generation '${generation}' cannot be represented by the Node.js public context.`
    );
  }
  return Number(generation);
}
