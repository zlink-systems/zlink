import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import { createZLinkRequest } from '../messaging/request-result';
import type { ZLinkRequest } from '../../contracts/Handlers/Attributes';
import type {
  ZLinkChannelClient,
  ZLinkChannelRequestCall,
  ZLinkFanoutClient,
  ZLinkFanoutPublishCall,
  ZLinkMessageMetadata,
  ZLinkPublishCall,
  ZLinkRequestCall,
  ZLinkRouteClient,
  ZLinkSendCall,
  ZLinkSpotPublisherClient
} from '../../contracts';
import type { SpotHandle } from '../spots/spot-handle';
import { ZLinkConfigurationException, type ZLinkFrameworkRegistration } from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import {
  requireOneWayCompletion,
  requirePublishCompletion,
  requireClassicFanoutCompletion,
  throwAlreadySubmitted,
  ZLinkSubmitStatus,
  submittedResult,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type {
  ZLinkChannelClientTransport,
  ZLinkChannelClientTransportSource,
  ZLinkRouteClientTransport,
  ZLinkSpotPublisherClientTransport
} from './channel-transports';
import { throwIfAborted } from '../abort';
import {
  captureZLinkSpotSerialTurn,
  requireZLinkYieldTurn,
  type ZLinkSpotSerialTurn
} from '../execution';
import { requirePublicFanoutTopic } from './fanout-service-wire';
import {
  requestToSpotHandle,
  sendToSpotHandle,
  type ZLinkSpotRoutedTransport
} from '../spots/spot-outbound';
import { resolveFrameworkPacketName } from '../messaging/packet-name';

type RouteMeshChannelMatch = {
  readonly meshName: string;
  readonly mesh: NonNullable<ReturnType<ZLinkFrameworkRegistration['spotNodes']['get']>>;
};

type ChannelRouteResolution =
  | {
      readonly kind: 'client-server';
      readonly channel: NonNullable<ReturnType<ZLinkFrameworkRegistration['channels']['get']>>;
    }
  | { readonly kind: 'route-mesh'; readonly matches: readonly RouteMeshChannelMatch[] };

type ResolvedChannelRoute =
  | Extract<ChannelRouteResolution, { readonly kind: 'client-server' }>
  | {
      readonly kind: 'route-mesh';
      readonly meshName: string;
      readonly mesh: RouteMeshChannelMatch['mesh'];
    };

export function resolveChannelRoute(
  registration: ZLinkFrameworkRegistration,
  channelName: string
): ChannelRouteResolution | undefined {
  const clientServerChannel = registration.channels.get(channelName);
  if (clientServerChannel !== undefined) {
    return { kind: 'client-server', channel: clientServerChannel };
  }

  const matches = [...registration.spotNodes.entries()]
    .filter(([, mesh]) =>
      Object.prototype.hasOwnProperty.call(mesh.meshChannels ?? {}, channelName)
    )
    .map(([meshName, mesh]) => ({ meshName, mesh }));
  return matches.length === 0 ? undefined : { kind: 'route-mesh', matches };
}

export function requireUniqueRouteMeshChannel(
  matches: readonly RouteMeshChannelMatch[],
  duplicateMessage: string
): RouteMeshChannelMatch {
  if (matches.length !== 1) {
    throw new ZLinkConfigurationException(duplicateMessage);
  }
  return matches[0]!;
}

export class DefaultZLinkChannelClient implements ZLinkChannelClient {
  private readonly routeClient: DefaultZLinkRouteClient;

  constructor(
    registration: ZLinkFrameworkRegistration,
    channelTransport?: ZLinkChannelClientTransportSource,
    routeTransport?: ZLinkRouteClientTransport,
    spotRouterChannelIdForMesh: (meshName: string) => string = (meshName) => meshName
  ) {
    this.routeClient = new DefaultZLinkRouteClient(
      registration,
      routeTransport,
      spotRouterChannelIdForMesh,
      channelTransport
    );
  }

  sendToChannel(channelName: string, message: unknown): ZLinkSendCall {
    return this.routeClient.sendToChannel(channelName, message);
  }

  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall {
    return this.routeClient.requestToChannel(channelName, request);
  }
}

export class DefaultZLinkFanoutClient implements ZLinkFanoutClient {
  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly transport?: ZLinkChannelClientTransportSource
  ) {}

  publish(channelName: string, event: unknown): ZLinkFanoutPublishCall;
  publish(channelName: string, topic: string, event: unknown): ZLinkFanoutPublishCall;
  publish(
    channelName: string,
    topicOrEvent: string | unknown,
    explicitEvent?: unknown
  ): ZLinkFanoutPublishCall {
    const hasExplicitTopic = arguments.length === 3;
    const event = hasExplicitTopic ? explicitEvent : topicOrEvent;
    const topic = hasExplicitTopic
      ? (topicOrEvent as string)
      : resolveFrameworkPacketName(event, undefined, 'Fanout');
    requirePublicFanoutTopic(topic);
    const packetName = resolveFrameworkPacketName(event, undefined, 'Fanout');
    return new DefaultZLinkFanoutPublishCall(
      () => this.requirePublisherChannel(channelName),
      async (signal) => ({
        status: (
          await this.requireTransport().publish(channelName, topic, packetName, event, signal)
        ).status
      })
    );
  }

  private requirePublisherChannel(channelName: string): void {
    if (!this.registration.fanoutPublishers.has(channelName)) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' does not have a publisher capability.`
      );
    }
  }

  private requireTransport(): ZLinkChannelClientTransport {
    const transport = typeof this.transport === 'function' ? this.transport() : this.transport;
    if (transport === undefined) {
      throw new ZLinkConfigurationException('Channel runtime is not started.');
    }
    return transport;
  }
}

interface PreparedNodeSend {
  readonly validate: () => void;
  readonly submit: (
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    message?: unknown
  ) => ZLinkSubmitResult | Promise<ZLinkSubmitResult>;
}

export class DefaultZLinkRouteClient implements ZLinkRouteClient {
  private readonly nodeSends = new Map<string, Map<string, PreparedNodeSend>>();
  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly transport?: ZLinkRouteClientTransport,
    private readonly spotRouterChannelIdForMesh: (meshName: string) => string = (meshName) =>
      meshName,
    private readonly channelTransport?: ZLinkChannelClientTransportSource
  ) {}

  sendToNode(meshName: string, targetNodeRid: string, message: unknown): ZLinkSendCall {
    let targets = this.nodeSends.get(meshName);
    if (targets === undefined) this.nodeSends.set(meshName, (targets = new Map()));
    let prepared = targets.get(targetNodeRid);
    if (prepared === undefined) {
      prepared = {
        validate: () => {
          this.requireMesh(meshName);
        },
        submit: (packetName, metadata, payload) =>
          normalizeSubmitResult(
            this.requireTransport().submit(meshName, targetNodeRid, packetName, payload, metadata)
          )
      };
      targets.set(targetNodeRid, prepared);
    }
    return new DefaultZLinkSendCall(prepared.validate, prepared.submit, message);
  }

  requestToNode(meshName: string, targetNodeRid: string, request: unknown): ZLinkRequestCall {
    return new DefaultZLinkRequestCall(
      () => this.requireMesh(meshName),
      (packetName, timeoutMs, metadata, signal, onAdmitted) =>
        this.requireTransport().request(
          meshName,
          targetNodeRid,
          packetName,
          request,
          timeoutMs,
          signal,
          metadata,
          onAdmitted
        ),
      this.defaultRequestTimeout(meshName)
    );
  }

  sendToChannel(channelName: string, message: unknown): ZLinkSendCall {
    return new DefaultZLinkSendCall(
      () => this.requireChannelRoute(channelName),
      async (packetName, metadata) => {
        const route = this.requireChannelRoute(channelName);
        if (route.kind === 'route-mesh') {
          return normalizeSubmitResult(
            await this.requireTransport().submitToChannel(
              route.meshName,
              channelName,
              packetName,
              message,
              metadata
            )
          );
        }
        return normalizeSubmitResult(
          await this.requireChannelTransport().send(channelName, packetName, message, metadata)
        );
      }
    );
  }

  requestToChannel(channelName: string, request: unknown): ZLinkChannelRequestCall {
    return new DefaultZLinkRequestCall(
      () => this.requireChannelRoute(channelName),
      (packetName, timeoutMs, metadata, signal, onAdmitted) => {
        const route = this.requireChannelRoute(channelName);
        if (route.kind === 'route-mesh') {
          return this.requireTransport().requestToChannel(
            route.meshName,
            channelName,
            packetName,
            request,
            timeoutMs,
            signal,
            metadata,
            onAdmitted
          );
        }
        return this.requireChannelTransport().request(
          channelName,
          packetName,
          request,
          timeoutMs,
          signal,
          metadata,
          onAdmitted
        );
      },
      this.defaultRequestTimeoutForChannel(channelName)
    );
  }

  sendToSpot(spot: SpotHandle, message: unknown): ZLinkSendCall {
    return new DefaultZLinkSendCall(
      () => {
        this.requireSpotTransport();
      },
      async (_packetName, metadata) => {
        await sendToSpotHandle(this.requireSpotTransport(), spot, message, {
          metadata,
          spotRouterChannelIdForMesh: this.spotRouterChannelIdForMesh
        });
        return { status: ZLinkSubmitStatus.Submitted };
      }
    );
  }

  requestToSpot(spot: SpotHandle, request: unknown): ZLinkRequestCall {
    return new DefaultZLinkRequestCall(
      () => {
        this.requireSpotTransport();
      },
      (_packetName, timeoutMs, metadata, signal, onAdmitted) =>
        requestToSpotHandle(this.requireSpotTransport(), spot, request, {
          timeoutMs,
          signal,
          metadata,
          onAdmitted,
          spotRouterChannelIdForMesh: this.spotRouterChannelIdForMesh
        }),
      this.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS
    );
  }

  private defaultRequestTimeout(meshName: string): number {
    return (
      this.registration.spotNodes.get(meshName)?.requestTimeoutMs ??
      this.registration.routeChannelOptions.get(meshName)?.requestTimeoutMs ??
      this.registration.requestTimeoutMs ??
      DEFAULT_REQUEST_TIMEOUT_MS
    );
  }

  private requireMesh(meshName: string): void {
    if (
      !this.registration.spotNodes.has(meshName) &&
      !this.registration.routeChannels.has(meshName)
    ) {
      throw new ZLinkConfigurationException(`RouteMesh '${meshName}' is not registered.`);
    }
  }

  private requireChannelRoute(channelName: string): ResolvedChannelRoute {
    const resolution = resolveChannelRoute(this.registration, channelName);
    if (resolution === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RequestTargetNotFound,
        `ChannelName '${channelName}' is not registered.`
      );
    }
    if (resolution.kind === 'client-server') {
      if (resolution.channel.client === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.InvalidConfiguration,
          `Channel client role '${channelName}' is not configured.`
        );
      }
      return resolution;
    }

    return {
      kind: 'route-mesh',
      ...requireUniqueRouteMeshChannel(
        resolution.matches,
        `RouteMesh channel '${channelName}' must be unique across the Framework host.`
      )
    };
  }

  private defaultRequestTimeoutForChannel(channelName: string): number {
    const route = resolveChannelRoute(this.registration, channelName);
    const timeoutMs =
      route?.kind === 'client-server'
        ? route.channel.requestTimeoutMs
        : route?.kind === 'route-mesh'
          ? route.matches[0]?.mesh.requestTimeoutMs
          : undefined;
    return timeoutMs ?? this.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS;
  }

  private requireTransport(): ZLinkRouteClientTransport {
    if (this.transport === undefined) {
      throw new ZLinkConfigurationException('Route channel runtime is not started.');
    }
    return this.transport;
  }

  private requireChannelTransport(): ZLinkChannelClientTransport {
    const transport =
      typeof this.channelTransport === 'function' ? this.channelTransport() : this.channelTransport;
    if (transport === undefined) {
      throw new ZLinkConfigurationException('Channel runtime is not started.');
    }
    return transport;
  }

  private requireSpotTransport(): ZLinkSpotRoutedTransport {
    const transport = this.requireTransport();
    if (transport.sendToSpot === undefined || transport.requestToSpot === undefined) {
      throw new ZLinkConfigurationException(
        'Route channel runtime does not support SpotHandle messaging.'
      );
    }
    return transport as ZLinkSpotRoutedTransport;
  }
}

export class DefaultZLinkSpotPublisherClient implements ZLinkSpotPublisherClient {
  constructor(
    private readonly registration: ZLinkFrameworkRegistration,
    private readonly transport?: ZLinkSpotPublisherClientTransport
  ) {}

  publish(meshName: string, channelName: string, topic: string, event: unknown): ZLinkPublishCall {
    return new DefaultZLinkPublishCall(
      () => this.requireMeshChannel(meshName, channelName),
      (packetName, metadata, signal) =>
        this.requireTransport().publish(
          meshName,
          channelName,
          topic,
          packetName,
          event,
          signal,
          metadata
        )
    );
  }

  private requireMeshChannel(meshName: string, channelName: string): void {
    const mesh = this.registration.spotNodes.get(meshName);
    if (
      mesh === undefined ||
      !Object.prototype.hasOwnProperty.call(mesh.meshChannels ?? {}, channelName)
    ) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' is not registered in RouteMesh '${meshName}'.`
      );
    }
  }

  private requireTransport(): ZLinkSpotPublisherClientTransport {
    if (this.transport === undefined) {
      throw new ZLinkConfigurationException('SPOT publisher runtime is not started.');
    }
    return this.transport;
  }
}

class DefaultZLinkSendCall implements ZLinkSendCall {
  private selectedMetadata?: Map<string, string>;
  private executed = false;

  constructor(
    private readonly validate: () => void,
    private readonly submitter: (
      packetName: string | undefined,
      metadata: ReadonlyMap<string, string>,
      message?: unknown
    ) => ZLinkSubmitResult | Promise<ZLinkSubmitResult>,
    private readonly message?: unknown
  ) {}

  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  metadata(keyOrMetadata: string | ZLinkMessageMetadata, value?: string): this {
    ensureNotExecuted(this.executed);
    this.selectedMetadata ??= new Map<string, string>();
    if (typeof keyOrMetadata === 'string') {
      this.selectedMetadata.set(keyOrMetadata, value!);
    } else {
      for (const [key, selectedValue] of keyOrMetadata.values) {
        this.selectedMetadata.set(key, selectedValue);
      }
    }
    return this;
  }

  submit(): Promise<void> {
    try {
      ensureNotExecuted(this.executed);
      this.validate();
      this.executed = true;
      const result = this.submitter(
        undefined,
        this.selectedMetadata ?? EMPTY_CALL_METADATA,
        this.message
      );
      if (result instanceof Promise) return result.then(completeOneWaySend);
      completeOneWaySend(result);
      return completedOneWaySend;
    } catch (error) {
      return Promise.reject(error);
    }
  }
}

function ensureNotExecuted(executed: boolean): void {
  if (executed) {
    throwAlreadySubmitted('ZLink call');
  }
}

const EMPTY_CALL_METADATA: ReadonlyMap<string, string> = new Map();
const completedOneWaySend = Promise.resolve();

function completeOneWaySend(result: ZLinkSubmitResult): void {
  requireOneWayCompletion(result, 'One-way send');
}

function normalizeSubmitResult(
  result: void | ZLinkSubmitResult | Promise<void | ZLinkSubmitResult>
): ZLinkSubmitResult | Promise<ZLinkSubmitResult> {
  return result instanceof Promise
    ? result.then((value) => value ?? submittedResult)
    : (result ?? submittedResult);
}

class DefaultZLinkRequestCall implements ZLinkChannelRequestCall {
  private timeoutMs?: number;
  private selectedMetadata?: Map<string, string>;
  private readonly turn: ZLinkSpotSerialTurn | undefined = captureZLinkSpotSerialTurn();

  constructor(
    private readonly validate: () => void,
    private readonly submitter: <TReply>(
      packetName: string | undefined,
      timeoutMs: number | undefined,
      metadata: ReadonlyMap<string, string>,
      signal?: AbortSignal,
      onAdmitted?: () => void
    ) => Promise<TReply>,
    private readonly defaultRequestTimeoutMs?: number
  ) {}

  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  metadata(keyOrMetadata: string | ZLinkMessageMetadata, value?: string): this {
    this.selectedMetadata ??= new Map<string, string>();
    if (typeof keyOrMetadata === 'string') {
      this.selectedMetadata.set(keyOrMetadata, value!);
    } else {
      for (const [key, selectedValue] of keyOrMetadata.values) {
        this.selectedMetadata.set(key, selectedValue);
      }
    }
    return this;
  }

  timeout(timeoutMs: number): this {
    this.timeoutMs = timeoutMs;
    return this;
  }

  submit<TReply>(signal?: AbortSignal): ZLinkRequest<TReply> {
    return createZLinkRequest((onAdmitted) => {
      throwIfAborted(signal);
      this.validate();
      return this.submitter<TReply>(
        undefined,
        this.timeoutMs ?? this.defaultRequestTimeoutMs,
        this.selectedMetadata === undefined ? EMPTY_CALL_METADATA : new Map(this.selectedMetadata),
        signal,
        onAdmitted
      );
    });
  }

  async yield<TReply>(signal?: AbortSignal): Promise<TReply> {
    const turn = requireZLinkYieldTurn(this.turn);
    throwIfAborted(signal);
    this.validate();
    const pending = this.submitter<TReply>(
      undefined,
      this.timeoutMs ?? this.defaultRequestTimeoutMs,
      this.selectedMetadata === undefined ? EMPTY_CALL_METADATA : new Map(this.selectedMetadata),
      signal
    );
    return turn.yieldPromise(pending);
  }
}

class DefaultZLinkPublishCall implements ZLinkPublishCall {
  private readonly selectedMetadata = new Map<string, string>();
  private executed = false;

  constructor(
    private readonly validate: () => void,
    private readonly submitter: (
      packetName: string | undefined,
      metadata: ReadonlyMap<string, string>,
      signal?: AbortSignal
    ) => ZLinkSubmitResult | Promise<ZLinkSubmitResult>
  ) {}

  metadata(key: string, value: string): this;
  metadata(metadata: ZLinkMessageMetadata): this;
  metadata(keyOrMetadata: string | ZLinkMessageMetadata, value?: string): this {
    ensureNotExecuted(this.executed);
    if (typeof keyOrMetadata === 'string') {
      this.selectedMetadata.set(keyOrMetadata, value!);
    } else {
      for (const [key, selectedValue] of keyOrMetadata.values) {
        this.selectedMetadata.set(key, selectedValue);
      }
    }
    return this;
  }

  async submit(signal?: AbortSignal): Promise<void> {
    ensureNotExecuted(this.executed);
    this.validate();
    this.executed = true;
    throwIfAborted(signal);
    const result = await this.submitter(undefined, new Map(this.selectedMetadata), signal);
    requirePublishCompletion(result, 'Logical Multicast publish');
  }
}

class DefaultZLinkFanoutPublishCall implements ZLinkFanoutPublishCall {
  private executed = false;

  constructor(
    private readonly validate: () => void,
    private readonly submitter: (signal?: AbortSignal) => Promise<ZLinkSubmitResult>
  ) {}

  async submit(signal?: AbortSignal): Promise<void> {
    ensureNotExecuted(this.executed);
    this.validate();
    this.executed = true;
    throwIfAborted(signal);
    const result = await this.submitter(signal);
    requireClassicFanoutCompletion(result, 'Classic fanout publish');
  }
}
