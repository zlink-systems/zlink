import type {
  RoutingId,
  SpotId,
  Type,
  ZLinkActor,
  ZLinkEntrySpot,
  ZLinkEntrySpotContext,
  ZLinkInstanceSpot,
  ZLinkInstanceSpotContext,
  ZLinkInstanceSpotHandlerRegistry,
  ZLinkSpot,
  ZLinkSpotContext,
  ZLinkSpotHandlerRegistry,
  ZLinkSpotOutbound,
  ZLinkSpotRelocationReadyCall,
  ZLinkSpotTimerHandler,
  ZLinkTimerOptions
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import type { ZLinkWorkerCall } from '../../contracts';
import { ZLinkConfigurationException } from '../configuration';
import { DefaultZLinkWorkerCall, ZLinkWorkerRuntime } from '../workers';
import {
  createTimerDiagnostics,
  type ZLinkSpotTimerRegistry
} from './spot-timer';
import type { ZLinkSpotSerialExecutor } from './spot-serial-executor';

interface ZLinkEntrySpotContextOptions {
  readonly spotId: SpotId;
  readonly objectGeneration: number;
  readonly nodeRid: RoutingId;
  readonly handlers: ZLinkSpotHandlerRegistry;
  readonly outbound: ZLinkSpotOutbound;
  readonly timers: ZLinkSpotTimerRegistry;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly getEntrySpot: () => ZLinkEntrySpot;
  readonly spotNodeName: string;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly workerRuntime: ZLinkWorkerRuntime;
  readonly destroyActor?: (
    nodeRid: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ) => Promise<void>;
}

interface ZLinkSpotContextOptions {
  readonly meshName: string;
  readonly spotId: SpotId;
  readonly objectGeneration: number;
  readonly handlers: ZLinkSpotHandlerRegistry;
  readonly outbound: ZLinkSpotOutbound;
  readonly timers: ZLinkSpotTimerRegistry;
  readonly serial: ZLinkSpotSerialExecutor;
  readonly getSpot: () => ZLinkSpot | undefined;
  readonly nodeRid?: RoutingId;
  readonly nodeRidProvider?: () => RoutingId | undefined;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly workerRuntime: ZLinkWorkerRuntime;
  readonly close: (signal?: AbortSignal) => Promise<boolean>;
  readonly leaveActor: (actor: ZLinkActor, signal?: AbortSignal) => Promise<void>;
  readonly relocationReady?: () => ZLinkSpotRelocationReadyCall;
  readonly ensureOperationAllowed?: () => void;
}

export function createEntrySpotContext(options: ZLinkEntrySpotContextOptions): ZLinkEntrySpotContext {
  return withImmutableSpotIdentity({
    meshName: options.spotNodeName,
    spotId: options.spotId,
    objectGeneration: options.objectGeneration,
    nodeRid: options.nodeRid,
    handlers: options.handlers,
    outbound: options.outbound,
    destroyActor(actor: ZLinkActor, signal?: AbortSignal) {
      if (options.destroyActor === undefined) {
        throw new ZLinkConfigurationException('Entry Spot actor destroy runtime is not started.');
      }
      return options.destroyActor(options.nodeRid, actor, signal);
    },
    addTimer<THandler extends ZLinkSpotTimerHandler<ZLinkEntrySpot>>(
      name: string,
      periodMs: number,
      handlerType: Type<THandler>,
      timerOptions?: ZLinkTimerOptions,
      signal?: AbortSignal
    ) {
      return options.timers.add(
        name,
        periodMs,
        timerOptions,
        handlerType,
        options.serial,
        options.getEntrySpot(),
        options.providerResolver,
        signal,
        createTimerDiagnostics(
          options.spotNodeName,
          options.spotId,
          true,
          name,
          handlerType,
          options.runtimeEventPublisher
        )
      );
    },
    runCpuWorker<T>(work: (signal: AbortSignal) => T): ZLinkWorkerCall<T> {
      return new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleCpu(work, timeoutMs, signal));
    },
    runIoWorker<T>(work: (signal: AbortSignal) => Promise<T>): ZLinkWorkerCall<T> {
      return new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleIo(work, timeoutMs, signal));
    }
  });
}

export function createSpotContext(options: ZLinkSpotContextOptions): ZLinkSpotContext {
  return withImmutableSpotIdentity({
    meshName: options.meshName,
    spotId: options.spotId,
    objectGeneration: options.objectGeneration,
    nodeRid: contextNodeRid(options.nodeRidProvider?.() ?? options.nodeRid),
    handlers: options.handlers,
    get outbound() {
      options.ensureOperationAllowed?.();
      return options.outbound;
    },
    relocationReady: () => {
      options.ensureOperationAllowed?.();
      return options.relocationReady?.() ?? {
        defer() {
          throw new ZLinkConfigurationException(
            'Spot relocation readiness is not configured for application signaling.'
          );
        }
      };
    },
    leaveActor: (actor: ZLinkActor, signal?: AbortSignal) => {
      options.ensureOperationAllowed?.();
      return options.leaveActor(actor, signal);
    },
    close: (signal?: AbortSignal) => {
      options.ensureOperationAllowed?.();
      return options.close(signal);
    },
    addTimer: <THandler extends ZLinkSpotTimerHandler<ZLinkSpot>>(
      name: string,
      periodMs: number,
      handlerType: Type<THandler>,
      timerOptions?: ZLinkTimerOptions,
      signal?: AbortSignal
    ) => {
      options.ensureOperationAllowed?.();
      const spot = options.getSpot();
      if (spot === undefined) {
        throw new ZLinkConfigurationException('Spot timer cannot be registered before spot activation.');
      }
      return options.timers.add(
        name,
        periodMs,
        timerOptions,
        handlerType,
        options.serial,
        spot,
        options.providerResolver,
        signal,
        createTimerDiagnostics(String(options.spotId), options.spotId, false, name, handlerType, options.runtimeEventPublisher)
      );
    },
    runCpuWorker: <T>(work: (signal: AbortSignal) => T): ZLinkWorkerCall<T> => {
      options.ensureOperationAllowed?.();
      return new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleCpu(work, timeoutMs, signal));
    },
    runIoWorker: <T>(work: (signal: AbortSignal) => Promise<T>): ZLinkWorkerCall<T> => {
      options.ensureOperationAllowed?.();
      return new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleIo(work, timeoutMs, signal));
    }
  });
}

export function createInstanceSpotContext(
  options: Omit<ZLinkSpotContextOptions, 'handlers' | 'leaveActor'> & {
    readonly handlers: ZLinkInstanceSpotHandlerRegistry;
  }
): ZLinkInstanceSpotContext {
  const common = {
    meshName: options.meshName,
    spotId: options.spotId,
    objectGeneration: options.objectGeneration,
    nodeRid: contextNodeRid(options.nodeRidProvider?.() ?? options.nodeRid),
    handlers: options.handlers,
    outbound: options.outbound,
    close: options.close,
    addTimer: <THandler extends ZLinkSpotTimerHandler<ZLinkInstanceSpot>>(
      name: string,
      periodMs: number,
      handlerType: Type<THandler>,
      timerOptions?: ZLinkTimerOptions,
      signal?: AbortSignal
    ) => {
      const spot = options.getSpot();
      if (spot === undefined) {
        throw new ZLinkConfigurationException('Instance Spot timer cannot be registered before activation.');
      }
      return options.timers.add(
        name,
        periodMs,
        timerOptions,
        handlerType,
        options.serial,
        spot,
        options.providerResolver,
        signal,
        createTimerDiagnostics(
          String(options.spotId),
          options.spotId,
          false,
          name,
          handlerType,
          options.runtimeEventPublisher
        )
      );
    },
    runCpuWorker: <T>(work: (signal: AbortSignal) => T): ZLinkWorkerCall<T> =>
      new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleCpu(work, timeoutMs, signal)),
    runIoWorker: <T>(work: (signal: AbortSignal) => Promise<T>): ZLinkWorkerCall<T> =>
      new DefaultZLinkWorkerCall(options.serial, (timeoutMs, signal) =>
        options.workerRuntime.scheduleIo(work, timeoutMs, signal))
  };
  return withImmutableSpotIdentity(common);
}

function contextNodeRid(nodeRid: RoutingId | undefined): RoutingId {
  return nodeRid ?? ('' as RoutingId);
}

function withImmutableSpotIdentity<T extends {
  readonly meshName: string;
  readonly spotId: SpotId;
  readonly objectGeneration: number;
  readonly nodeRid: RoutingId;
}>(context: T): T {
  for (const key of ['meshName', 'spotId', 'objectGeneration', 'nodeRid'] as const) {
    Object.defineProperty(context, key, {
      configurable: false,
      enumerable: true,
      writable: false,
      value: context[key]
    });
  }
  return context;
}
