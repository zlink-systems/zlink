import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import { ZLinkFrameworkRuntimeState, ZLinkSpotKind } from '../../contracts';
import { ZLinkFrameworkException } from '../../contracts/Errors';
import type {
  RoutingId,
  ZLinkChannelClient,
  ZLinkChannelRequestCall,
  ZLinkFanoutClient,
  ZLinkPublishCall,
  ZLinkRequestCall,
  ZLinkSendCall,
  ZLinkMessageMetadata,
  ZLinkSpotOutbound,
  ZLinkSpotRequestCall,
  ZLinkSpotSendCall,
  ZLinkSpotPublisherClient
} from '../../contracts';
import {
  requireOneWayCompletion,
  throwAlreadySubmitted,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import { normalizeOpaqueRoutingId } from '../routing-id';
import { ZLinkConfigurationException } from '../configuration';
import type { ZLinkBackendSpot } from '../backend/contracts';
import { deliverOnSerial } from '../workers';
import {
  requireZLinkYieldTurn
} from '../execution';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import type { ZLinkSpotRouteTarget } from './spot-routing-internal';
import { ZLinkSpotSerialExecutor } from './spot-serial-executor';
import {
  resolveSpotHandle,
  refreshSpotHandle,
  type SpotHandle,
  type ResolvedSpotHandle
} from './spot-handle';

export class DefaultZLinkSpotOutbound implements ZLinkSpotOutbound {
  constructor(
    private readonly serial: ZLinkSpotSerialExecutor,
    private readonly channelClient?: ZLinkChannelClient,
    _fanoutClient?: ZLinkFanoutClient,
    private readonly spotPublisherClient?: ZLinkSpotPublisherClient,
    private readonly routedTransport?: ZLinkSpotRoutedTransport,
    private readonly spotRouterChannelIdForMesh: (meshName: string) => string = (meshName) => meshName,
    private readonly sourceSpotProvider?: () => ZLinkBackendSpot | undefined,
    private readonly meshName?: string,
    _channelMeshNameForChannel?: (channelName: string) => string | undefined,
    private readonly addressTransport?: ZLinkSpotAddressTransport
  ) {}

  sendToSpot(spotId: RoutingId, message: unknown): ZLinkSpotSendCall;
  /** @internal Compatibility path for an already resolved handle. */
  sendToSpot(spot: SpotHandle, message: unknown): ZLinkSendCall;
  sendToSpot(spot: RoutingId | SpotHandle, message: unknown): ZLinkSpotSendCall | ZLinkSendCall {
    if (typeof spot !== 'object') {
      return createAddressedSpotSendCall(
        this.serial,
        this.requireAddressTransport(),
        spot,
        message,
        this.sourceSpotProvider
      );
    }
    return wrapRoutedSpotSendCall(
      this.serial,
      this.requireRoutedTransport(),
      spot,
      message,
      this.spotRouterChannelIdForMesh,
      this.sourceSpotProvider
    );
  }

  requestToSpot(spotId: RoutingId, request: unknown): ZLinkSpotRequestCall;
  /** @internal Compatibility path for an already resolved handle. */
  requestToSpot(spot: SpotHandle, request: unknown): ZLinkRequestCall;
  requestToSpot(spot: RoutingId | SpotHandle, request: unknown): ZLinkSpotRequestCall | ZLinkRequestCall {
    if (typeof spot !== 'object') {
      return createAddressedSpotRequestCall(
        this.serial,
        this.requireAddressTransport(),
        spot,
        request,
        this.sourceSpotProvider
      );
    }
    return wrapRoutedSpotRequestCall(
      this.serial,
      this.requireRoutedTransport(),
      spot,
      request,
      this.spotRouterChannelIdForMesh,
      this.sourceSpotProvider
    );
  }

  publish(channelName: string, topic: string, event: unknown): ZLinkPublishCall {
    if (this.spotPublisherClient !== undefined) {
      return wrapPublishCall(
        this.serial,
        this.spotPublisherClient.publish(this.requireMeshName(), channelName, topic, event)
      );
    }
    throw new ZLinkConfigurationException('Spot outbound publish requires a configured Spot publisher client.');
  }

  sendToChannel(channelName: string, message: unknown): ZLinkSendCall {
    return wrapSendCall(
      this.serial,
      this.requireChannelClient().sendToChannel(channelName, message)
    );
  }

  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall {
    return wrapRequestCall(
      this.serial,
      this.requireChannelClient().requestToChannel(channelName, request)
    );
  }

  private requireChannelClient(): ZLinkChannelClient {
    if (this.channelClient === undefined) {
      throw new ZLinkConfigurationException('Spot channel outbound runtime is not started.');
    }
    return this.channelClient;
  }

  private requireMeshName(): string {
    if (this.meshName === undefined || this.meshName.length === 0) {
      throw new ZLinkConfigurationException('Spot channel outbound requires a MeshName context.');
    }
    return this.meshName;
  }

  private requireRoutedTransport(): ZLinkSpotRoutedTransport {
    if (this.routedTransport === undefined) {
      throw new ZLinkConfigurationException('Spot routed outbound runtime is not started.');
    }
    return this.routedTransport;
  }

  private requireAddressTransport(): ZLinkSpotAddressTransport {
    if (this.addressTransport === undefined) {
      throw new ZLinkConfigurationException('Spot address outbound runtime is not started.');
    }
    return this.addressTransport;
  }
}

export interface ZLinkSpotAddressCallOptions {
  readonly metadata?: ReadonlyMap<string, string>;
  readonly instanceSpot: boolean;
  readonly instanceSpotType?: string;
  readonly initialMeshName?: string;
  readonly timeoutMs?: number;
  readonly signal?: AbortSignal;
  readonly sourceSpot?: ZLinkBackendSpot;
}

/**
 * Resolves current authority and owns Missing Instance placement and
 * activation. Public callers never handle an owner or generation fence.
 */
export interface ZLinkSpotAddressTransport {
  sendToSpotAddress(
    spotId: RoutingId,
    message: unknown,
    options: ZLinkSpotAddressCallOptions
  ): Promise<ZLinkSubmitResult>;
  requestToSpotAddress<TReply = unknown>(
    spotId: RoutingId,
    request: unknown,
    options: ZLinkSpotAddressCallOptions
  ): Promise<TReply>;
}

export interface ZLinkSpotRoutedTransport {
  sendToSpot(
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: ZLinkSpotRoutedSendOptions
  ): Promise<ZLinkSubmitResult>;
  sendFromSpotToSpot?(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    message: unknown,
    options: ZLinkSpotRoutedSendOptions
  ): Promise<ZLinkSubmitResult>;
  requestToSpot<TReply = unknown>(
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: ZLinkSpotRoutedRequestOptions
  ): Promise<TReply>;
  requestFromSpotToSpot?<TReply = unknown>(
    sourceSpot: ZLinkBackendSpot,
    spotRouteTarget: ZLinkSpotRouteTarget,
    request: unknown,
    options: ZLinkSpotRoutedRequestOptions
  ): Promise<TReply>;
}

export interface ZLinkSpotRoutedSendOptions {
  readonly packetName?: string;
  /** Internal end-to-end budget remaining for route admission. */
  readonly timeoutMs?: number;
  readonly signal?: AbortSignal;
  readonly metadata?: ReadonlyMap<string, string>;
}

export interface ZLinkSpotRoutedRequestOptions extends ZLinkSpotRoutedSendOptions {
  readonly timeoutMs?: number;
}

interface MutableAddressCallOptions {
  readonly metadata: Map<string, string>;
  metadataPresent: boolean;
  instanceSpot: boolean;
  instanceSpotType?: string;
  initialMeshName?: string;
  timeoutMs?: number;
  submitted: boolean;
  readonly selected: Set<string>;
}

function createAddressedSpotSendCall(
  serial: ZLinkSpotSerialExecutor,
  transport: ZLinkSpotAddressTransport,
  spotId: RoutingId,
  message: unknown,
  sourceSpotProvider?: () => ZLinkBackendSpot | undefined
): ZLinkSpotSendCall {
  const options = newAddressCallOptions();
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      addMetadata(options, key, value);
      return this;
    },
    instanceSpot(instanceSpotType?: string) {
      selectOnce(options, 'instanceSpot');
      options.instanceSpot = true;
      if (instanceSpotType !== undefined) {
        options.instanceSpotType = requireAddressValue(
          instanceSpotType,
          'Instance Spot type',
          255
        );
      }
      return this;
    },
    inMesh(meshName: string) {
      selectOnce(options, 'inMesh');
      options.initialMeshName = requireAddressValue(meshName, 'Mesh name', 255);
      return this;
    },
    async submit(signal?: AbortSignal): Promise<void> {
      markSubmitted(options);
      const pending = startRequestOnSerial(serial, () => ({
        pending: transport.sendToSpotAddress(
          spotId,
          message,
          freezeAddressCallOptions(options, signal, sourceSpotProvider?.())
        )
      }));
      const result = await (serial.isCurrentTurn ? pending : deliverOnSerial(serial, pending));
      requireOneWayCompletion(
        result,
        'Spot send',
        ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      );
    }
  };
}

function createAddressedSpotRequestCall(
  serial: ZLinkSpotSerialExecutor,
  transport: ZLinkSpotAddressTransport,
  spotId: RoutingId,
  request: unknown,
  sourceSpotProvider?: () => ZLinkBackendSpot | undefined
): ZLinkSpotRequestCall {
  const options = newAddressCallOptions();
  const begin = <TReply>(signal?: AbortSignal) => {
    markSubmitted(options);
    rejectSameSpotRequest(serial, spotId);
    return startRequestOnSerial<TReply>(serial, () => ({
      pending: transport.requestToSpotAddress<TReply>(
        spotId,
        request,
        freezeAddressCallOptions(options, signal, sourceSpotProvider?.())
      )
    }));
  };
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      addMetadata(options, key, value);
      return this;
    },
    instanceSpot(instanceSpotType?: string) {
      selectOnce(options, 'instanceSpot');
      options.instanceSpot = true;
      if (instanceSpotType !== undefined) {
        options.instanceSpotType = requireAddressValue(
          instanceSpotType,
          'Instance Spot type',
          255
        );
      }
      return this;
    },
    inMesh(meshName: string) {
      selectOnce(options, 'inMesh');
      options.initialMeshName = requireAddressValue(meshName, 'Mesh name', 255);
      return this;
    },
    timeout(timeoutMs: number) {
      selectOnce(options, 'timeout');
      if (!Number.isSafeInteger(timeoutMs) || timeoutMs <= 0) {
        throw new ZLinkConfigurationException('Spot request timeout must be a positive safe integer.');
      }
      options.timeoutMs = timeoutMs;
      return this;
    },
    submit<TReply>(signal?: AbortSignal) {
      const pending = begin<TReply>(signal);
      return serial.isCurrentTurn ? pending : deliverOnSerial(serial, pending);
    },
    yield<TReply>(signal?: AbortSignal) {
      const turn = requireZLinkYieldTurn();
      const pending = begin<TReply>(signal);
      return turn.yieldPromise(pending);
    }
  };
}

function newAddressCallOptions(): MutableAddressCallOptions {
  return {
    metadata: new Map(),
    metadataPresent: false,
    instanceSpot: false,
    submitted: false,
    selected: new Set()
  };
}

function selectOnce(options: MutableAddressCallOptions, name: string): void {
  if (options.submitted) {
    throwAlreadySubmitted('Spot call');
  }
  if (options.selected.has(name)) {
    throw new ZLinkConfigurationException(`Spot call option '${name}' was already selected.`);
  }
  options.selected.add(name);
}

function addMetadata(
  options: MutableAddressCallOptions,
  key: string | ZLinkMessageMetadata,
  value?: string
): void {
  if (options.submitted) {
    throwAlreadySubmitted('Spot call');
  }
  options.metadataPresent = true;
  if (typeof key === 'string') {
    options.metadata.set(key, value!);
    return;
  }
  for (const [metadataKey, metadataValue] of key.values) {
    options.metadata.set(metadataKey, metadataValue);
  }
}

function markSubmitted(options: MutableAddressCallOptions): void {
  if (options.submitted) {
    throwAlreadySubmitted('Spot call');
  }
  if (!options.instanceSpot && (
    options.instanceSpotType !== undefined
    || options.initialMeshName !== undefined
  )) {
    throw new ZLinkConfigurationException(
      'Mesh selection requires an Instance Spot intent.'
    );
  }
  options.submitted = true;
}

function freezeAddressCallOptions(
  options: MutableAddressCallOptions,
  signal: AbortSignal | undefined,
  sourceSpot: ZLinkBackendSpot | undefined
): ZLinkSpotAddressCallOptions {
  return {
    ...(options.metadataPresent ? { metadata: new Map(options.metadata) } : {}),
    instanceSpot: options.instanceSpot,
    instanceSpotType: options.instanceSpotType,
    initialMeshName: options.initialMeshName,
    timeoutMs: options.timeoutMs,
    signal,
    sourceSpot
  };
}

function requireAddressValue(value: string, label: string, maxBytes: number): string {
  const byteLength = Buffer.byteLength(value, 'utf8');
  if (byteLength < 1 || byteLength > maxBytes || value.includes('\0')) {
    throw new ZLinkConfigurationException(
      `${label} must contain 1..${maxBytes} UTF-8 bytes and no NUL.`
    );
  }
  return value;
}

function wrapSendCall(serial: ZLinkSpotSerialExecutor, inner: ZLinkSendCall): ZLinkSendCall {
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      if (typeof key === 'string') inner.metadata(key, value!);
      else inner.metadata(key);
      return this;
    },
    async submit(signal?: AbortSignal) {
      const result = await serial.execute(() => inner.submit(signal));
      return result;
    }
  };
}

function wrapPublishCall(serial: ZLinkSpotSerialExecutor, inner: ZLinkPublishCall): ZLinkPublishCall {
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      if (typeof key === 'string') inner.metadata(key, value!);
      else inner.metadata(key);
      return this;
    },
    submit(signal?: AbortSignal) {
      return serial.execute(() => inner.submit(signal));
    }
  };
}

function wrapRequestCall(
  serial: ZLinkSpotSerialExecutor,
  inner: ZLinkChannelRequestCall
): ZLinkChannelRequestCall {
  const begin = <TReply>(signal?: AbortSignal) =>
    startRequestOnSerial(serial, () => ({ pending: inner.submit<TReply>(signal) }));
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      if (typeof key === 'string') inner.metadata(key, value!);
      else inner.metadata(key);
      return this;
    },
    timeout(timeoutMs: number) {
      inner.timeout(timeoutMs);
      return this;
    },
    submit<TReply>(signal?: AbortSignal) {
      const pending = begin<TReply>(signal);
      return serial.isCurrentTurn ? pending : deliverOnSerial(serial, pending);
    },
    yield<TReply>(signal?: AbortSignal) {
      const turn = requireZLinkYieldTurn();
      const pending = begin<TReply>(signal);
      return turn.yieldPromise(pending);
    }
  };
}

/**
 * Starts an outbound request in Spot serial order without gating the serial
 * line on the request round trip. When already running inside the Spot line
 * (a handler turn), the request starts immediately as part of that turn;
 * otherwise the start is enqueued as its own serial turn.
 */
function startRequestOnSerial<TReply>(
  serial: ZLinkSpotSerialExecutor,
  begin: () => Promise<{ pending: Promise<TReply> }> | { pending: Promise<TReply> }
): Promise<TReply> {
  return serial.execute(begin).then((startedRequest) => startedRequest.pending);
}

function wrapRoutedSpotSendCall(
  serial: ZLinkSpotSerialExecutor,
  transport: ZLinkSpotRoutedTransport,
  spot: SpotHandle,
  message: unknown,
  spotRouterChannelIdForMesh: (meshName: string) => string,
  sourceSpotProvider?: () => ZLinkBackendSpot | undefined
): ZLinkSendCall {
  const metadata = new Map<string, string>();
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      if (typeof key === 'string') {
        metadata.set(key, value!);
      } else {
        for (const [selectedKey, selectedValue] of key.values) {
          metadata.set(selectedKey, selectedValue);
        }
      }
      return this;
    },
    async submit(signal?: AbortSignal): Promise<void> {
      const pending = startRequestOnSerial(serial, () => ({
        pending: sendToSpotHandle(
          transport,
          spot,
          message,
          {
            metadata,
            signal,
            spotRouterChannelIdForMesh,
            sourceSpot: sourceSpotProvider?.()
          }
        )
      }));
      const result = await (serial.isCurrentTurn ? pending : deliverOnSerial(serial, pending));
      requireOneWayCompletion(
        result,
        'Spot send',
        ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      );
    }
  };
}

function wrapRoutedSpotRequestCall(
  serial: ZLinkSpotSerialExecutor,
  transport: ZLinkSpotRoutedTransport,
  spot: SpotHandle,
  request: unknown,
  spotRouterChannelIdForMesh: (meshName: string) => string,
  sourceSpotProvider?: () => ZLinkBackendSpot | undefined
): ZLinkRequestCall & Pick<ZLinkChannelRequestCall, 'yield'> {
  let selectedTimeoutMs: number | undefined;
  const metadata = new Map<string, string>();
  const begin = <TReply>(signal?: AbortSignal) => {
    // A request to the current Spot would wait behind the handler that owns
    // the same Spot claim. Reject it before resolving a route or submitting
    // transport work; one-way self-send remains a valid FIFO admission.
    rejectSameSpotRequest(serial, spot.spotId);
    return startRequestOnSerial<TReply>(serial, () => ({
      pending: requestToSpotHandle<TReply>(transport, spot, request, {
        timeoutMs: selectedTimeoutMs,
        signal,
        metadata,
        spotRouterChannelIdForMesh,
        sourceSpot: sourceSpotProvider?.()
      })
    }));
  };
  return {
    metadata(key: string | ZLinkMessageMetadata, value?: string) {
      if (typeof key === 'string') {
        metadata.set(key, value!);
      } else {
        for (const [metadataKey, metadataValue] of key.values) {
          metadata.set(metadataKey, metadataValue);
        }
      }
      return this;
    },
    timeout(timeoutMs: number) {
      selectedTimeoutMs = timeoutMs;
      return this;
    },
    submit<TReply>(signal?: AbortSignal) {
      const pending = begin<TReply>(signal);
      return serial.isCurrentTurn ? pending : deliverOnSerial(serial, pending);
    },
    yield<TReply>(signal?: AbortSignal) {
      const turn = requireZLinkYieldTurn();
      const pending = begin<TReply>(signal);
      return turn.yieldPromise(pending);
    }
  };
}

function rejectSameSpotRequest(serial: ZLinkSpotSerialExecutor, targetSpotId: RoutingId): void {
  const sourceSpotId = serial.sourceSpotId;
  if (sourceSpotId === undefined || String(sourceSpotId) !== String(targetSpotId)) return;
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.InvalidOperation,
    `A Spot request cannot await the current Spot '${String(sourceSpotId)}' while holding its execution claim.`
  );
}

export interface ZLinkSpotHandleCallOptions {
  readonly metadata?: ReadonlyMap<string, string>;
  readonly timeoutMs?: number;
  readonly signal?: AbortSignal;
  readonly spotRouterChannelIdForMesh?: (meshName: string) => string;
  readonly sourceSpot?: ZLinkBackendSpot;
}

export async function sendToSpotHandle(
  transport: ZLinkSpotRoutedTransport,
  spot: SpotHandle,
  message: unknown,
  options: ZLinkSpotHandleCallOptions = {}
): Promise<ZLinkSubmitResult> {
  const packetName = resolveFrameworkPacketName(message, undefined, 'SPOT');
  const sendResolved = async (resolved: ResolvedSpotHandle): Promise<ZLinkSubmitResult> => {
    const target = spotRefToSpotRouteTarget(
      resolved,
      options.spotRouterChannelIdForMesh
    );
    if (options.sourceSpot !== undefined && transport.sendFromSpotToSpot !== undefined) {
      return await transport.sendFromSpotToSpot(options.sourceSpot, target, message, {
        packetName,
        timeoutMs: options.timeoutMs,
        signal: options.signal,
        metadata: options.metadata
      });
    }
    return await transport.sendToSpot(target, message, {
      packetName,
      timeoutMs: options.timeoutMs,
      signal: options.signal,
      metadata: options.metadata
    });
  };

  return await sendResolved(await requireSpotRef(spot, options.signal));
}

export async function requestToSpotHandle<TReply = unknown>(
  transport: ZLinkSpotRoutedTransport,
  spot: SpotHandle,
  request: unknown,
  options: ZLinkSpotHandleCallOptions = {}
): Promise<TReply> {
  const packetName = resolveFrameworkPacketName(request, undefined, 'SPOT');
  const requestResolved = async (resolved: ResolvedSpotHandle): Promise<TReply> => {
    const target = spotRefToSpotRouteTarget(resolved, options.spotRouterChannelIdForMesh);
    const transportOptions = {
      packetName,
      timeoutMs: options.timeoutMs,
      signal: options.signal,
      metadata: options.metadata
    };
    if (options.sourceSpot !== undefined && transport.requestFromSpotToSpot !== undefined) {
      return await transport.requestFromSpotToSpot<TReply>(
        options.sourceSpot,
        target,
        request,
        transportOptions
      );
    }
    return await transport.requestToSpot<TReply>(target, request, transportOptions);
  };

  const resolved = await requireSpotRef(spot, options.signal);
  try {
    return await requestResolved(resolved);
  } catch (error) {
    if (!shouldRefreshAfterRouteDisconnect(resolved, error)) {
      throw error;
    }
    let refreshed: ResolvedSpotHandle | undefined;
    try {
      refreshed = await refreshSpotHandle(spot, options.signal);
    } catch {
      throw error;
    }
    if (isShutdownTargetState(refreshed?.targetNodeState)) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
        `Spot '${String(spot.spotId)}' target host is shutting down.`,
        false,
        error
      );
    }
    throw error;
  }
}

async function requireSpotRef(handle: SpotHandle, signal?: AbortSignal): Promise<ResolvedSpotHandle> {
  const resolved = await resolveSpotHandle(handle, signal);
  if (resolved === undefined) {
    throw new ZLinkConfigurationException(`Spot '${handle.spotId}' has no live location.`);
  }
  return resolved;
}

function spotRefToSpotRouteTarget(
  spot: ResolvedSpotHandle,
  spotRouterChannelIdForMesh: (meshName: string) => string = (meshName) => meshName
): ZLinkSpotRouteTarget {
  return {
    routerChannelId: spotRouterChannelIdForMesh(spot.meshName),
    targetNodeRid: normalizeSpotRefRoutingId(spot.nodeRid),
    spotId: normalizeSpotRefRoutingId(spot.spotId),
    spotKind: spot.spotKind ?? ZLinkSpotKind.User,
    targetSpotGeneration: spot.spotGeneration,
    targetNodeState: spot.targetNodeState
  };
}

function shouldRefreshAfterRouteDisconnect(
  target: ResolvedSpotHandle,
  error: unknown
): boolean {
  return target.targetNodeState === ZLinkFrameworkRuntimeState.Serving
    && error instanceof ZLinkFrameworkException
    && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.RouteNotConnected;
}

function isShutdownTargetState(
  state: ResolvedSpotHandle['targetNodeState']
): boolean {
  return state === ZLinkFrameworkRuntimeState.Draining
    || state === ZLinkFrameworkRuntimeState.Stopped;
}

function normalizeSpotRefRoutingId(routingId: RoutingId): RoutingId {
  return normalizeOpaqueRoutingId(routingId);
}
