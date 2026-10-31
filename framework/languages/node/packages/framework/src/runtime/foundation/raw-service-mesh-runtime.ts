import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import type { ConnectionAttempt } from '@zlink-systems/zlink';
import { admitServiceRequest } from './durable-service-request';
import { remainingRequestTimeout, requestTerminalError } from '../messaging/request-deadline';
import type { OperationHandle } from './operation-registry';
import type { RequestRuntime } from './request-runtime';
import { inspectRuntime } from '../internal-observation';
import { constants as osConstants } from 'node:os';
import { ApplicationIngressRecordOwner } from '../application-jobs/application-ingress-record-owner';
import type {
  ApplicationJobPermitPort,
  ApplicationJobReceiveRotationPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import type {
  ZLinkRawBindingPort,
  ZLinkRawHostPort,
  ZLinkRawReceivedRecord,
  ZLinkRawRouterPort
} from '../backend/raw-binding-port';
import { submitToRequestResult } from '../messaging/submission-result';
import { RequestResult, SubmitResult, isZLinkBackendResultError } from '../backend/runtime-values';
import { enumWireRejectReason } from '../protocol/service_wire_codec.generated';
import { SERVICE_WIRE_PREFIX_SIZE } from './service-wire-binary-primitives';
import {
  OperationRegistry,
  OperationTimeoutError,
  type OperationObserver,
  type PendingOperation
} from './operation-registry';
import { ServiceLivenessRegistry, type ServiceLivenessTick } from './service-liveness-registry';
import { ServiceMailbox, type ServiceMailboxRecord } from './service-mailbox';
import {
  ServiceTopologyRegistry,
  validateDescriptor,
  type AdmittedServicePeer,
  type PeerAdmissionResult,
  type ServiceNodeDescriptor,
  type ServicePeerAdmissionExpectation
} from './service-topology-registry';
import { createServiceWireCodec } from './service-wire-codec';
import {
  SERVICE_WIRE_MAGIC,
  SERVICE_WIRE_MAJOR,
  ServiceWireCommand,
  ServiceWireFrameworkErrorCode
} from './service-wire-constants.generated';
import {
  decodeApplicationPayloadView,
  decodeChannelRequestHeader,
  decodeChannelSendHeader,
  decodeHeader,
  decodeNodeRequestHeader,
  decodeReject,
  decodeReplyHeader,
  decodeRouteMeshAdmission,
  encodeApplicationPayload,
  encodeChannelRequestHeader,
  encodeChannelSendHeader,
  encodeNodeRequestHeader,
  encodeNodeSendHeader,
  encodeReject,
  encodeReplyHeader,
  encodeRouteMeshAdmission,
  M6aServiceWireCommand,
  ServiceWireProtocolError,
  type ServiceApplicationPayload
} from './service-wire-m6a-codec';

const nativeErrnoValues = osConstants.errno;

export type RawServicePumpResult =
  'noData' | 'infrastructure' | 'application' | 'dropped' | 'protocolError';

export type RawServicePumpObserver = (sourceRoutingId: string, byteCount: number) => void;

export interface RawServiceRequestResult {
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly payload?: ServiceApplicationPayload;
}

/** Receives observed request terminals; the context is the caller's operation kind. */
export type RawServiceRequestObserver = OperationObserver<RawServiceRequestResult, number>;

/**
 * A registered request. An observed remote request has no Promise; its
 * terminal reaches the observer. Local and Promise-contract requests carry
 * the Promise.
 */
export interface RawServiceObservedRequest extends OperationHandle<RawServiceRequestResult> {}

type RawServiceChannelTargetSelection =
  | { readonly kind: 'selected'; readonly peer: AdmittedServicePeer }
  | {
      readonly kind: 'noMember';
      readonly submitResult: typeof SubmitResult.NotFound;
      readonly requestResult: RawServiceRequestResult;
    }
  | {
      readonly kind: 'knownButNotReady';
      readonly submitResult: typeof SubmitResult.NotConnected;
      readonly requestResult: RawServiceRequestResult;
    };

/** An M6A application frame already owned by the Framework runtime. */
type ServiceApplicationPayloadInput = ServiceApplicationPayload | Buffer;

export interface RawServiceIngressRecord {
  /** Source identity recorded once by the first accepting owner (Location runtime §9.3). */
  requestSource?: import('../locations/request-source-resolver').ZLinkRequestSourceFence;
  readonly onAdmitted?: () => void;
  readonly command: number;
  readonly flags: number;
  readonly sourceRoutingId: string;
  readonly sourceNodeGeneration?: bigint;
  readonly sourceRoute?: Uint8Array;
  readonly requestSequence?: bigint;
  readonly reply?: ServiceMailboxRecord['reply'];
  readonly parts: readonly Buffer[];
  readonly applicationJobOwner?: ApplicationIngressRecordOwner;
}

export type RawServiceIngressHandler = (
  record: RawServiceIngressRecord
) => RawServicePumpResult | undefined | Promise<RawServicePumpResult | undefined>;

export interface RawServiceMeshRuntimeOptions {
  readonly requests?: RequestRuntime;
  readonly onReceiveReady?: () => void;
  readonly infrastructureTaskRunner?: import('../spots/spot-actor-join-dispatch').ZLinkDetachedTaskRunner;
  readonly onPendingOperationsChanged?: () => void;
  readonly descriptor: ServiceNodeDescriptor;
  readonly resolveAdvertisedEndpoint?: (boundEndpoint: string) => string;
  readonly probeIntervalMs?: number;
  readonly peerTimeoutMs?: number;
  readonly bindingPort: ZLinkRawBindingPort;
  readonly applicationJobQueue?: ApplicationJobQueuePort;
  readonly peerAdmissionSealed?: () => boolean;
  readonly onReceiveFlowConfigFailure?: (error: unknown) => void;
  readonly onMailboxReady?: (domain: 'application' | 'infrastructure') => void;
  readonly onPeerNotRequired?: (nodeRoutingId: string, endpoint: string) => void;
  readonly onPeerDisconnected?: (
    nodeRoutingId: string,
    endpoint: string,
    lifecycleGeneration: bigint
  ) => void;
  readonly onProtocolError?: (record: {
    readonly sourceRoutingId: string;
    readonly request: boolean;
    readonly replied: boolean;
    readonly command?: number;
  }) => void;
}

// Framework error code 13 (RequestTargetNotFound) is encoded as 14 on a
// RequestResult.NotFound reply. Boundary transport results keep failureCode 0.
const RAW_MESH_RECEIVE_RECORD_BUDGET = 64;
const NODE_SEND_HEADER = encodeNodeSendHeader();
const RAW_MESH_RECEIVE_BYTE_BUDGET = 4 * 1024 * 1024;
export const RAW_MESH_RECEIVE_TIME_BUDGET_MS = 2;

const REQUEST_TARGET_NOT_FOUND_FAILURE_CODE = ServiceWireFrameworkErrorCode.requestTargetNotFound;

const livenessCodec = createServiceWireCodec({
  magic: SERVICE_WIRE_MAGIC,
  major: SERVICE_WIRE_MAJOR,
  commands: M6aServiceWireCommand
});

interface ControlSendState {
  task?: Promise<boolean>;
  next?: () => boolean | Promise<boolean>;
  // HELLO send eligibility; admission suspension does not end the exchange.
  current?: () => boolean;
  // Admission owns the first HELLO until ADMIT or route termination.
  hello?: Uint8Array;
}

/**
 * RouteMesh M6A runtime built only on the public raw binding package.
 * It owns protocol, admission, mailbox, request and liveness state.
 */
export class RawServiceMeshRuntime {
  [inspectRuntime](nodeRoutingId?: string) {
    return Object.freeze({
      pendingControlSends: this.controlSends.size,
      selectedRouteGeneration:
        nodeRoutingId === undefined ? undefined : this.selectedRoutes.get(nodeRoutingId),
      peerRouteObservers: this.peerRouteChanged.size,
      peerConnectionIntentObservers: this.peerConnectionIntentRemoved.size,
      expectedPeers: this.expectedPeers.size
    });
  }

  readonly topology: ServiceTopologyRegistry;
  readonly mailbox: ServiceMailbox;
  readonly liveness: ServiceLivenessRegistry;

  private readonly requestOwner: RequestRuntime | undefined;
  private readonly releaseOperations: () => void;

  private readonly operations: OperationRegistry<RawServiceRequestResult, number>;
  private readonly expectedPeers = new Map<
    string,
    {
      readonly meshName: string;
      readonly nodeRoutingId: string;
      readonly endpoint?: string;
      readonly securityIdentity?: string;
      readonly lifecycleGeneration?: bigint;
    }
  >();
  private readonly endpointOnlyPeers = new Set<string>();
  private readonly peerConnectionIntentRemoved = new Set<(nodeRoutingId: string) => void>();
  private readonly peerRouteChanged = new Set<(nodeRoutingId: string) => void>();
  /**
   * The last observed Core selected route of each RID (Core ROUTER §10.1):
   * RID -> route generation. Only `observeSelectedRoutes` replaces it.
   */
  private selectedRoutes = new Map<string, bigint>();
  private connectionAttempts: readonly ConnectionAttempt[] = [];
  private readonly bindingPort: ZLinkRawBindingPort;
  private readonly applicationJobQueue: ApplicationJobQueuePort;
  private readonly nodeMailboxOwner: string;
  private readonly peerAdmissionSealed?: () => boolean;
  private readonly applicationJobStop = new AbortController();
  private receiveAcquisition?: Promise<ApplicationJobPermitPort>;
  private receivePermit?: ApplicationJobPermitPort;
  private receiveRotation?: ApplicationJobReceiveRotationPort;
  private readonly controlSends = new Map<string, ControlSendState>();
  private infrastructureFailure?: { readonly error: unknown };
  private readonly onReceiveReady?: () => void;
  private readonly infrastructureTaskRunner?: RawServiceMeshRuntimeOptions['infrastructureTaskRunner'];
  private readonly onPeerNotRequired?: RawServiceMeshRuntimeOptions['onPeerNotRequired'];
  private readonly onPeerDisconnected?: RawServiceMeshRuntimeOptions['onPeerDisconnected'];
  private readonly onProtocolError?: RawServiceMeshRuntimeOptions['onProtocolError'];
  private readonly onReceiveFlowConfigFailure?: RawServiceMeshRuntimeOptions['onReceiveFlowConfigFailure'];
  private readonly resolveAdvertisedEndpoint?: RawServiceMeshRuntimeOptions['resolveAdvertisedEndpoint'];
  private descriptor: ServiceNodeDescriptor;
  private host?: ZLinkRawHostPort;
  private router?: ZLinkRawRouterPort;
  private nextCorrelation = 1n;
  private serviceIngress?: RawServiceIngressHandler;
  private serviceIngressAccepts?: (command: number) => boolean;
  private closed = false;

  constructor(options: RawServiceMeshRuntimeOptions) {
    this.requestOwner = options.requests;
    this.onReceiveReady = options.onReceiveReady;
    this.infrastructureTaskRunner = options.infrastructureTaskRunner;
    this.operations =
      options.requests?.operations<RawServiceRequestResult, number>() ??
      new OperationRegistry(undefined, options.onPendingOperationsChanged, (reason) =>
        requestTerminalError('Request', reason)
      );
    this.releaseOperations =
      options.requests === undefined
        ? () => this.operations.close('Raw service runtime closed.')
        : () => {};
    this.descriptor = options.descriptor;
    this.nodeMailboxOwner = `node:${options.descriptor.nodeRoutingId}`;
    this.topology = new ServiceTopologyRegistry(options.descriptor);
    this.mailbox = new ServiceMailbox(options.onMailboxReady);
    this.liveness = new ServiceLivenessRegistry(options.probeIntervalMs, options.peerTimeoutMs);
    this.bindingPort = options.bindingPort;
    if (options.applicationJobQueue === undefined) {
      throw new TypeError('Raw service runtime requires the host Application Job Queue.');
    }
    this.applicationJobQueue = options.applicationJobQueue;
    this.peerAdmissionSealed = options.peerAdmissionSealed;
    this.onPeerNotRequired = options.onPeerNotRequired;
    this.onPeerDisconnected = options.onPeerDisconnected;
    this.onProtocolError = options.onProtocolError;
    this.onReceiveFlowConfigFailure = options.onReceiveFlowConfigFailure;
    this.resolveAdvertisedEndpoint = options.resolveAdvertisedEndpoint;
  }

  start(): void {
    if (this.router !== undefined) return;
    if (this.closed) throw new Error('Raw service runtime cannot restart after close.');
    const host = this.bindingPort.createHost();
    let router: ZLinkRawRouterPort | undefined;
    try {
      router = host.createRouter();
      const receiveFlowRouter = router;
      router.setRoutingId(this.descriptor.nodeRoutingId);
      this.applicationJobQueue.registerReceiveFlowTarget?.(
        router,
        (state) => receiveFlowRouter.setReceiveFlowState(state),
        this.onReceiveFlowConfigFailure
      );
      router.bind(this.descriptor.advertisedEndpoint);
      const boundEndpoint = router.localEndpoint();
      const next = {
        ...this.descriptor,
        advertisedEndpoint: this.resolveAdvertisedEndpoint?.(boundEndpoint) ?? boundEndpoint,
        descriptorRevision: this.descriptor.descriptorRevision + 1n,
        state: 'serving' as const
      };
      this.topology.publishLocal(next);
      this.descriptor = next;
      this.host = host;
      this.router = router;
    } catch (error) {
      if (router !== undefined) {
        this.applicationJobQueue.unregisterReceiveFlowTarget?.(router);
      }
      host.close();
      throw error;
    }
  }

  connectPeer(endpoint: string, expected: ServiceNodeDescriptor): void {
    this.requireStarted().connectToRoutingId(expected.nodeRoutingId, endpoint);
    this.expectedPeers.set(
      expected.nodeRoutingId,
      Object.assign(this.expectedPeers.get(expected.nodeRoutingId) ?? {}, {
        meshName: expected.meshName,
        nodeRoutingId: expected.nodeRoutingId,
        endpoint,
        securityIdentity: expected.securityIdentity,
        lifecycleGeneration: expected.lifecycleGeneration
      })
    );
    this.notifyPeerRouteChanged(expected.nodeRoutingId);
  }

  connectPeerByRoutingId(
    endpoint: string,
    nodeRoutingId: string,
    securityIdentity?: string,
    lifecycleGeneration?: bigint
  ): void {
    this.requireStarted().connectToRoutingId(nodeRoutingId, endpoint);
    this.expectedPeers.set(
      nodeRoutingId,
      Object.assign(this.expectedPeers.get(nodeRoutingId) ?? {}, {
        meshName: this.topology.localDescriptor().meshName,
        nodeRoutingId,
        endpoint,
        securityIdentity,
        lifecycleGeneration
      })
    );
    this.notifyPeerRouteChanged(nodeRoutingId);
  }

  expectPeerByRoutingId(
    endpoint: string,
    nodeRoutingId: string,
    securityIdentity?: string,
    lifecycleGeneration?: bigint
  ): void {
    this.expectedPeers.set(
      nodeRoutingId,
      Object.assign(this.expectedPeers.get(nodeRoutingId) ?? {}, {
        meshName: this.topology.localDescriptor().meshName,
        nodeRoutingId,
        endpoint,
        securityIdentity,
        lifecycleGeneration
      })
    );
    this.notifyPeerRouteChanged(nodeRoutingId);
  }

  connectPeerEndpoint(endpoint: string): void {
    this.requireStarted().connect(endpoint);
    this.endpointOnlyPeers.add(endpoint);
  }

  disconnectPeer(endpoint: string, nodeRoutingId: string, lifecycleGeneration?: bigint): void {
    const current = this.topology.peer(nodeRoutingId);
    const expected = this.expectedPeers.get(nodeRoutingId);
    if (
      lifecycleGeneration !== undefined &&
      // Discovery has already fenced this RID to a replacement process.
      // The old auto-connect intent must not use disconnectRid here: that
      // is RID-wide and can close the provisional replacement before its
      // Hello/admission promotes it. admitPeer owns exact old-pair teardown
      // after the replacement descriptor is accepted.
      ((expected?.lifecycleGeneration !== undefined &&
        expected.lifecycleGeneration !== lifecycleGeneration) ||
        (current !== undefined && current.descriptor.lifecycleGeneration !== lifecycleGeneration) ||
        (current === undefined && expected?.lifecycleGeneration !== lifecycleGeneration))
    ) {
      return;
    }
    try {
      const router = this.requireStarted();
      if (router.disconnectRid !== undefined) {
        router.disconnectRid(nodeRoutingId);
      } else {
        try {
          router.disconnect(endpoint);
        } catch (error) {
          if (!isAlreadyDisconnectedError(error)) throw error;
        }
      }
    } finally {
      if (current !== undefined) this.removePeer(current);
      this.topology.forgetNotRequired(nodeRoutingId);
      if (
        lifecycleGeneration === undefined ||
        expected?.lifecycleGeneration === undefined ||
        expected.lifecycleGeneration === lifecycleGeneration
      ) {
        this.expectedPeers.delete(nodeRoutingId);
      }
      this.notifyPeerConnectionIntentRemoved(nodeRoutingId);
    }
  }

  disconnectPeerEndpoint(endpoint: string): void {
    try {
      this.requireStarted().disconnect(endpoint);
    } catch (error) {
      if (!isAlreadyDisconnectedError(error)) throw error;
    } finally {
      this.endpointOnlyPeers.delete(endpoint);
      const removed = new Set<string>();
      for (const [nodeRoutingId, expected] of this.expectedPeers) {
        if (expected.endpoint === endpoint) {
          this.expectedPeers.delete(nodeRoutingId);
          removed.add(nodeRoutingId);
        }
      }
      for (const peer of this.topology.peers()) {
        if (peer.descriptor.advertisedEndpoint === endpoint) {
          this.removePeer(peer);
          removed.add(peer.descriptor.nodeRoutingId);
        }
      }
      for (const nodeRoutingId of removed) this.notifyPeerConnectionIntentRemoved(nodeRoutingId);
    }
  }

  observePeerConnectionIntentRemoved(listener: (nodeRoutingId: string) => void): () => void {
    this.peerConnectionIntentRemoved.add(listener);
    return () => {
      this.peerConnectionIntentRemoved.delete(listener);
    };
  }

  observePeerRoute(
    nodeRoutingId: string
  ): import('./durable-service-request').PeerRouteObservation {
    const selected = this.selectedRoutes.get(nodeRoutingId);
    const peer = this.topology.admittedPeer(nodeRoutingId);
    const expected = this.expectedPeers.get(nodeRoutingId);
    const admitted = peer?.liveness?.connectionId;
    return {
      generation: selected ?? (typeof admitted === 'bigint' ? admitted : undefined),
      ready: peer?.remoteAdmitReceived === true,
      selected: selected !== undefined,
      lifecycleGeneration: expected?.lifecycleGeneration ?? peer?.descriptor.lifecycleGeneration,
      endpoint: expected?.endpoint ?? peer?.descriptor.advertisedEndpoint,
      // The immutable socket snapshot is borrowed; only waiting admission filters it.
      connectionAttempts: this.connectionAttempts
    };
  }

  peerLifecycleGeneration(nodeRoutingId: string): bigint | undefined {
    return (
      this.expectedPeers.get(nodeRoutingId)?.lifecycleGeneration ??
      this.topology.admittedPeer(nodeRoutingId)?.descriptor.lifecycleGeneration
    );
  }

  async waitForPeerRoute(
    nodeRoutingId: string,
    lifecycleGeneration: bigint,
    deadlineUnixMs: bigint,
    signal?: AbortSignal
  ): Promise<void> {
    const remaining = deadlineUnixMs - BigInt(Date.now());
    if (remaining <= 0n) throw new OperationTimeoutError(0n);
    const pending = this.operations.register(Number(remaining));
    const check = () => {
      if (this.isPeerRouteReady(nodeRoutingId, lifecycleGeneration)) {
        pending.reply({ terminalResult: RequestResult.Ok, failureCode: 0 });
      }
    };
    const unsubscribe = this.observePeerRouteChanged((rid) => {
      if (rid === nodeRoutingId) check();
    });
    const abort = () => pending.replyFailure(signal?.reason);
    signal?.addEventListener('abort', abort, { once: true });
    try {
      if (signal?.aborted === true) abort();
      else check();
      await pending.promise;
    } finally {
      unsubscribe();
      signal?.removeEventListener('abort', abort);
    }
  }

  get requestStopSignal(): AbortSignal {
    return this.applicationJobStop.signal;
  }

  observePeerRouteChanged(listener: (nodeRoutingId: string) => void): () => void {
    this.peerRouteChanged.add(listener);
    return () => {
      this.peerRouteChanged.delete(listener);
    };
  }

  hasSelectedPeerRoute(nodeRoutingId: string): boolean {
    return this.selectedRoutes.has(nodeRoutingId);
  }

  private notifyPeerRouteChanged(nodeRoutingId: string): void {
    for (const listener of this.peerRouteChanged) listener(nodeRoutingId);
  }

  private notifyPeerConnectionIntentRemoved(nodeRoutingId: string): void {
    // Only the logical owner's removal publishes lifecycle termination.
    // A physical disconnect alone retains the connection expectation.
    if (this.expectedPeers.has(nodeRoutingId) || this.topology.peer(nodeRoutingId) !== undefined)
      return;
    for (const listener of this.peerConnectionIntentRemoved) listener(nodeRoutingId);
  }

  announcePeer(nodeRoutingId: string): boolean {
    if (this.closed) return false;
    const generation = this.selectedRoutes.get(nodeRoutingId);
    if (generation === undefined) return false;
    const key = controlSendKey(nodeRoutingId, M6aServiceWireCommand.hello);
    if (!this.isPeerAnnouncementCurrent(nodeRoutingId, generation)) {
      return false;
    }
    const current = () =>
      this.isPeerAnnouncementCurrent(nodeRoutingId, generation) &&
      this.peerAdmissionSealed?.() !== true;
    const existing = this.controlSends.get(key);
    if (!current()) {
      if (existing === undefined) this.controlSends.set(key, { current });
      return false;
    }
    const hello =
      existing?.hello ??
      encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor());
    return this.startControlSend(
      nodeRoutingId,
      M6aServiceWireCommand.hello,
      () => this.submitControl(nodeRoutingId, [hello]),
      current,
      hello
    );
  }

  private isPeerAnnouncementCurrent(nodeRoutingId: string, generation: bigint): boolean {
    return (
      this.selectedRoutes.get(nodeRoutingId) === generation &&
      !this.topology.hasRemoteAdmit(nodeRoutingId, routeConnectionId(generation))
    );
  }

  isPeerRouteReady(nodeRoutingId: string, lifecycleGeneration?: bigint): boolean {
    // Admission is bound to the observed selected route: the route observer
    // removes the peer when that route ends or is replaced.
    const peer = this.topology.readyPeer(nodeRoutingId);
    return (
      peer !== undefined &&
      (lifecycleGeneration === undefined ||
        peer.descriptor.lifecycleGeneration === lifecycleGeneration)
    );
  }

  async announceExpectedPeers(): Promise<number> {
    let accepted = 0;
    for (const nodeRoutingId of this.expectedPeers.keys()) {
      if (this.topology.peer(nodeRoutingId) !== undefined) continue;
      if (await this.announcePeer(nodeRoutingId)) accepted++;
    }
    return accepted;
  }

  async updateLocalDescriptor(options: {
    readonly placementWeight?: number;
    readonly channelName?: string;
    readonly channelWeight?: number;
    readonly state?: 'draining';
  }): Promise<void> {
    const current = this.topology.localDescriptor();
    const channels =
      options.state === 'draining'
        ? current.channels.map((channel) => ({ ...channel, weight: 0 }))
        : options.channelName === undefined
          ? current.channels
          : current.channels.map((channel) =>
              channel.name === options.channelName
                ? { ...channel, weight: options.channelWeight! }
                : channel
            );
    const next = {
      ...current,
      descriptorRevision: current.descriptorRevision + 1n,
      state: options.state ?? current.state,
      placementWeight: options.placementWeight ?? current.placementWeight,
      channels
    };
    this.topology.publishLocal(next);
    this.descriptor = next;
    // An admitted peer keeps the descriptor it received at admission.  The
    // placement and channel selectors therefore need the new mutable fields
    // on that same connection before the next request is selected; waiting for
    // a reconnect would leave a race where a drained node is selected again.
    await this.publishLocalDescriptor(
      this.topology.peers().map((peer) => peer.descriptor.nodeRoutingId)
    );
  }

  private async publishLocalDescriptor(targets: readonly string[]): Promise<void> {
    const update = encodeRouteMeshAdmission(
      M6aServiceWireCommand.update,
      this.topology.localDescriptor()
    );
    for (const target of targets) {
      await this.sendService(target, [update]);
    }
  }

  replaceDiscoveredNotRequired(descriptors: readonly ServiceNodeDescriptor[]): void {
    this.topology.replaceDiscoveredNotRequired(descriptors);
  }

  isObjectClientNodeDirectTarget(nodeRoutingId: string): boolean {
    return this.topology.isObjectClientNodeDirectTarget(nodeRoutingId);
  }

  sendToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput
  ): boolean | Promise<boolean> {
    return this.send(targetNodeRoutingId, [NODE_SEND_HEADER, this.applicationFrame(payload)]);
  }

  sendToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput
  ): SubmitResult | Promise<SubmitResult> {
    const selection = this.selectChannelTarget(channelName);
    if (selection.kind !== 'selected') return selection.submitResult;
    const selected = selection.peer;
    const applicationFrame = this.applicationFrame(payload);
    const submitted = this.send(selected.descriptor.nodeRoutingId, [
      encodeChannelSendHeader(channelName),
      applicationFrame
    ]);
    return typeof submitted === 'boolean'
      ? channelSendSubmitResult(submitted)
      : submitted.then(channelSendSubmitResult);
  }

  requestToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted?: () => void
  ): PendingOperation<RawServiceRequestResult>;
  requestToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted: (() => void) | undefined,
    observer: RawServiceRequestObserver,
    operationKind: number,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest;
  requestToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted?: () => void,
    observer?: RawServiceRequestObserver,
    operationKind?: number,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest {
    return this.requestToTarget(
      targetNodeRoutingId,
      payload,
      timeoutMs,
      undefined,
      onAdmitted,
      observer,
      operationKind,
      requestOperation
    );
  }

  requestToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted?: () => void
  ): PendingOperation<RawServiceRequestResult>;
  requestToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted: (() => void) | undefined,
    observer: RawServiceRequestObserver,
    operationKind: number,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest;
  requestToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    onAdmitted?: () => void,
    observer?: RawServiceRequestObserver,
    operationKind?: number,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest {
    const selection = this.selectChannelTarget(channelName);
    if (selection.kind !== 'selected') {
      const pending = this.registerRequest(timeoutMs, observer, operationKind, requestOperation);
      this.completeRequest(
        pending,
        selection.requestResult,
        requestOperation === undefined ? undefined : observer,
        operationKind
      );
      return pending;
    }
    return this.requestToTarget(
      selection.peer.descriptor.nodeRoutingId,
      payload,
      timeoutMs,
      channelName,
      onAdmitted,
      observer,
      operationKind
    );
  }

  beginInfrastructureRequest(
    timeoutMs: number,
    signal?: AbortSignal,
    absoluteDeadlineMs = performance.now() + timeoutMs
  ): PendingOperation<readonly Uint8Array[]> {
    const pending = this.operations.register(timeoutMs, 'registry', undefined, absoluteDeadlineMs);
    pending.setSignal(signal);
    return pending as unknown as PendingOperation<readonly Uint8Array[]>;
  }

  /**
   * One pending entry per request: an observed request reports its terminal
   * to the caller's observer; any other request keeps the Promise contract.
   */
  private registerRequest(
    timeoutMs: number,
    observer: RawServiceRequestObserver | undefined,
    operationKind: number | undefined,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest {
    if (requestOperation !== undefined) {
      return requestOperation as PendingOperation<RawServiceRequestResult>;
    }
    return observer === undefined
      ? this.operations.register(timeoutMs)
      : this.observedRequest(this.operations.registerObserved(timeoutMs, observer, operationKind));
  }

  private observedRequest(id: bigint): RawServiceObservedRequest {
    return this.operations.handle(id);
  }

  /**
   * An observed caller learns the operation id only when the request call
   * returns, so a terminal decided inside that call is delivered after it.
   */
  private completeRequest(
    pending: RawServiceObservedRequest,
    result: RawServiceRequestResult,
    observer?: RawServiceRequestObserver,
    operationKind?: number
  ): void {
    if (observer !== undefined) {
      queueMicrotask(() => observer.operationCompleted(pending.id, result, operationKind));
      return;
    }
    if (pending.promise === undefined || pending.hasObserver())
      queueMicrotask(() => pending.reply(result));
    else pending.reply(result);
  }

  private failRequest(
    pending: RawServiceObservedRequest,
    error: unknown,
    observer?: RawServiceRequestObserver,
    operationKind?: number
  ): void {
    if (observer !== undefined) {
      queueMicrotask(() => observer.operationFailed(pending.id, error, operationKind));
      return;
    }
    if (pending.promise === undefined) queueMicrotask(() => pending.replyFailure(error));
    else pending.replyFailure(error);
  }

  private selectChannelTarget(channelName: string): RawServiceChannelTargetSelection {
    const selected = this.topology.selectChannel(channelName);
    if (selected !== undefined) return { kind: 'selected', peer: selected };
    return this.topology.hasKnownChannelTarget(channelName)
      ? {
          kind: 'knownButNotReady',
          submitResult: SubmitResult.NotConnected,
          requestResult: { terminalResult: RequestResult.NotConnected, failureCode: 0 }
        }
      : {
          kind: 'noMember',
          submitResult: SubmitResult.NotFound,
          requestResult: {
            terminalResult: RequestResult.NotFound,
            failureCode: REQUEST_TARGET_NOT_FOUND_FAILURE_CODE
          }
        };
  }

  setServiceIngress(
    handler: RawServiceIngressHandler,
    accepts?: (command: number) => boolean
  ): void {
    if (this.serviceIngress !== undefined && this.serviceIngress !== handler) {
      throw new Error('Raw service ingress is already registered.');
    }
    this.serviceIngress = handler;
    this.serviceIngressAccepts = accepts;
  }

  async reserveLocalIngress(signal?: AbortSignal): Promise<ApplicationIngressRecordOwner> {
    const waitSignal =
      signal === undefined
        ? this.applicationJobStop.signal
        : AbortSignal.any([this.applicationJobStop.signal, signal]);
    const permit = await this.applicationJobQueue.acquire(waitSignal, 'local');
    return ApplicationIngressRecordOwner.create(this.applicationJobQueue, permit, { close() {} });
  }

  async sendService(targetNodeRoutingId: string, parts: readonly Uint8Array[]): Promise<boolean> {
    try {
      await this.requireStarted().send(targetNodeRoutingId, parts);
      return true;
    } catch (error) {
      return this.sendFailure(error);
    }
  }

  requestService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onAdmitted?: () => void,
    existing?: Pick<PendingOperation<unknown>, 'id' | 'deadlineMs'>,
    receiver?: undefined,
    onSubmitted?: () => void
  ): Promise<readonly Buffer[]>;
  requestService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onAdmitted: (() => void) | undefined,
    existing: Pick<PendingOperation<unknown>, 'id' | 'deadlineMs'> | undefined,
    receiver: import('./operation-registry').OperationObserver<readonly Buffer[], unknown>
  ): void;
  requestService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onAdmitted?: () => void,
    existing?: Pick<PendingOperation<unknown>, 'id' | 'deadlineMs'>,
    receiver?: import('./operation-registry').OperationObserver<readonly Buffer[], unknown>,
    onSubmitted?: () => void
  ): Promise<readonly Buffer[]> | void {
    if (receiver !== undefined) {
      try {
        this.requireStarted().request(
          targetNodeRoutingId,
          parts,
          timeoutMs,
          (reply) => receiver.operationCompleted(existing?.id ?? 0n, reply, undefined),
          (error) => receiver.operationFailed(existing?.id ?? 0n, error, undefined),
          this.topology.admittedPeer(targetNodeRoutingId)?.liveness?.recordReceived,
          onAdmitted,
          existing
        );
      } catch (error) {
        receiver.operationFailed(existing?.id ?? 0n, error, undefined);
      }
      return;
    }
    const admission = this.topology.admittedPeer(targetNodeRoutingId)?.liveness;
    try {
      const reply = this.requireStarted().request(
        targetNodeRoutingId,
        parts,
        timeoutMs,
        (reply) => reply,
        (error) => {
          throw error;
        },
        admission?.recordReceived,
        onAdmitted,
        existing
      );
      onSubmitted?.();
      return reply;
    } catch (error) {
      return Promise.reject(error);
    }
  }

  replyService(
    record: Pick<
      RawServiceIngressRecord,
      'sourceRoutingId' | 'sourceRoute' | 'requestSequence' | 'reply'
    >,
    parts: readonly (Uint8Array | string)[]
  ): void {
    if (record.requestSequence === undefined) {
      throw new TypeError('Service reply requires a request sequence.');
    }
    if (record.reply === undefined) {
      throw new TypeError('Service reply requires an opaque reply capability.');
    }
    record.reply(parts);
  }

  reply(
    request: ServiceMailboxRecord,
    payload: ServiceApplicationPayloadInput,
    terminalResult = 0,
    failureCode = 0
  ): void {
    if (request.localReply !== undefined) {
      request.localReply(
        terminalResult,
        failureCode,
        terminalResult === 0 ? this.applicationPayload(payload) : undefined
      );
      return;
    }
    if (request.reply === undefined || request.correlation === undefined) {
      throw new TypeError('Reply requires a request mailbox record.');
    }
    const header = encodeReplyHeader(request.correlation, terminalResult, failureCode);
    request.reply(terminalResult === 0 ? [header, this.applicationFrame(payload)] : [header]);
  }

  async pumpOne(
    nowMs = performance.now(),
    observe?: RawServicePumpObserver
  ): Promise<RawServicePumpResult> {
    const result = await this.receiveOne(nowMs, observe);
    const acquisition = this.receiveAcquisition;
    if (acquisition === undefined) return result;
    // A single receive operation may await its FIFO grant. Infrastructure
    // rounds use receiveOne directly and end while that grant is pending.
    try {
      await acquisition;
    } catch (error) {
      if (this.closed) return 'noData';
      throw error;
    }
    if (this.closed) return 'noData';
    return this.receiveOne(nowMs, observe);
  }

  setReadableHandler(
    handler: (receiveReady: boolean, routeReady: boolean, connectReady: boolean) => void
  ): void {
    this.requireStarted().setReadableHandler(handler);
  }

  /** Returns whether a receive budget ended before no-data; idle ticks only maintain peers. */
  pumpBatch(
    receiveReady = true,
    routeReady = false,
    connectReady = false
  ): boolean | Promise<boolean> {
    try {
      this.observeInfrastructureFailure();
      if (routeReady || connectReady) {
        return this.receiveBatchAfterObservations(receiveReady, routeReady, connectReady);
      }
      return this.receiveBatch(receiveReady);
    } catch (error) {
      return Promise.reject(error);
    }
  }

  private async receiveBatchAfterObservations(
    receiveReady: boolean,
    routeReady: boolean,
    connectReady: boolean
  ): Promise<boolean> {
    // Both facts are applied before admission observers run in this socket turn.
    if (connectReady) this.connectionAttempts = this.requireStarted().connectionAttemptsSnapshot();
    if (routeReady) await this.observeSelectedRoutes();
    if (connectReady) {
      for (const target of this.expectedPeers.keys()) this.notifyPeerRouteChanged(target);
      for (const peer of this.topology.peers()) {
        if (!this.expectedPeers.has(peer.descriptor.nodeRoutingId))
          this.notifyPeerRouteChanged(peer.descriptor.nodeRoutingId);
      }
    }
    return this.receiveBatch(receiveReady);
  }

  private receiveBatch(receiveReady: boolean): boolean | Promise<boolean> {
    const startedAtMs = performance.now();
    let messages = 0;
    let bytes = 0;
    const observe: RawServicePumpObserver = (_source, byteCount) => {
      bytes += byteCount;
    };
    const rotation = (this.receiveRotation ??= this.applicationJobQueue.createReceiveRotation(
      RAW_MESH_RECEIVE_RECORD_BUDGET,
      'remote'
    ));
    if (receiveReady) rotation.reserve();
    // The same loop owns both immediate receives and a suspended receive's
    // continuation; only the latter needs a Promise and retains this frame.
    const drain = (received?: RawServicePumpResult): boolean | Promise<boolean> => {
      try {
        while (
          received !== undefined ||
          (receiveReady && messages < RAW_MESH_RECEIVE_RECORD_BUDGET && !this.closed)
        ) {
          if (received === undefined) {
            const pending = this.receiveOne(performance.now(), observe, rotation);
            if (pending instanceof Promise) {
              return pending.then(drain, (error) => {
                rotation.releaseUnused();
                throw error;
              });
            }
            received = pending;
          }
          if (received === 'noData') {
            receiveReady = false;
            break;
          }
          messages += 1;
          received = undefined;
          // Core owns the per-peer fair-queue cursor. The Framework limit bounds
          // the entire round, so changing peers does not renew its byte/count budget.
          if (
            bytes >= RAW_MESH_RECEIVE_BYTE_BUDGET ||
            performance.now() - startedAtMs >= RAW_MESH_RECEIVE_TIME_BUDGET_MS
          )
            break;
        }
      } catch (error) {
        rotation.releaseUnused();
        throw error;
      }
      rotation.releaseUnused();
      if (!this.closed) {
        this.tickLiveness();
      }
      return receiveReady && messages > 0;
    };
    return drain();
  }

  private receiveOne(
    nowMs: number,
    observe?: RawServicePumpObserver,
    rotation?: ApplicationJobReceiveRotationPort
  ): RawServicePumpResult | Promise<RawServicePumpResult> {
    const router = this.requireStarted();
    this.observeInfrastructureFailure();
    let permit = this.receivePermit;
    this.receivePermit = undefined;
    permit ??= rotation?.take();
    if (permit === undefined && this.receiveAcquisition === undefined) {
      permit = this.acquireReceivePermit();
    }
    if (permit === undefined) return 'noData';
    if (this.closed) {
      permit.releaseAfterInternalProcessing();
      permit.releaseStorage?.();
      return 'noData';
    }
    let received: ZLinkRawReceivedRecord | undefined;
    try {
      received = router.receive(true);
    } catch (error) {
      permit.releaseAfterInternalProcessing();
      permit.releaseStorage?.();
      throw error;
    }
    if (received === undefined) {
      permit.releaseAfterInternalProcessing();
      permit.releaseStorage?.();
      return 'noData';
    }
    const applicationJobOwner = ApplicationIngressRecordOwner.create(
      this.applicationJobQueue,
      permit,
      received
    );
    let result: RawServicePumpResult | Promise<RawServicePumpResult>;
    try {
      result = this.processReceived(received, nowMs, applicationJobOwner);
    } catch (error) {
      applicationJobOwner.close();
      throw error;
    }
    return result instanceof Promise
      ? this.finishPendingReceive(result, received, applicationJobOwner, observe)
      : this.finishReceive(received, applicationJobOwner, observe, result);
  }

  private acquireReceivePermit(): ApplicationJobPermitPort | undefined {
    let resolveAcquisition: ((permit: ApplicationJobPermitPort) => void) | undefined;
    let rejectAcquisition: ((error: unknown) => void) | undefined;
    const permit = this.applicationJobQueue.acquireOrResume(
      (permit) => {
        this.receiveAcquisition = undefined;
        if (this.closed) {
          permit.releaseAfterInternalProcessing();
          permit.releaseStorage?.();
        } else {
          this.receivePermit = permit;
          this.onReceiveReady?.();
        }
        resolveAcquisition?.(permit);
      },
      (error) => rejectAcquisition?.(error),
      this.applicationJobStop.signal,
      'remote'
    );
    if (permit === undefined) {
      const acquisition = new Promise<ApplicationJobPermitPort>((resolve, reject) => {
        resolveAcquisition = resolve;
        rejectAcquisition = reject;
      });
      this.receiveAcquisition = acquisition;
      acquisition.catch((error) => {
        this.receiveAcquisition = undefined;
        if (!this.applicationJobStop.signal.aborted) this.reportInfrastructureFailure(error);
      });
    }
    return permit;
  }

  private finishPendingReceive(
    result: Promise<RawServicePumpResult>,
    received: ZLinkRawReceivedRecord,
    owner: ApplicationIngressRecordOwner,
    observe: RawServicePumpObserver | undefined
  ): Promise<RawServicePumpResult> {
    return result.then(
      (value) => this.finishReceive(received, owner, observe, value),
      (error) => {
        owner.close();
        throw error;
      }
    );
  }

  private finishReceive(
    received: ZLinkRawReceivedRecord,
    owner: ApplicationIngressRecordOwner,
    observe: RawServicePumpObserver | undefined,
    result: RawServicePumpResult
  ): RawServicePumpResult {
    try {
      if (result === 'protocolError') this.reportProtocolError(received);
      if (observe !== undefined) {
        let bytes = 0;
        for (let index = 0; index < received.parts.length; index += 1)
          bytes += received.parts[index]!.byteLength;
        observe(received.sourceRid, bytes);
      }
      return result;
    } finally {
      owner.close();
    }
  }

  private reportProtocolError(
    received: import('../backend/node/node-raw-binding-port').ZLinkRawReceivedRecord
  ): void {
    const request = received.requestSeq !== undefined;
    const replied = request && this.replyGenericProtocolError(received);
    this.onProtocolError?.({
      sourceRoutingId: received.sourceRid,
      request,
      replied,
      ...protocolCommand(received.parts)
    });
  }

  private replyGenericProtocolError(
    received: import('../backend/node/node-raw-binding-port').ZLinkRawReceivedRecord
  ): boolean {
    if (received.requestSeq === undefined || received.parts.length === 0) return false;
    try {
      const header = decodeHeader(received.parts[0]!);
      const correlation =
        header.command === M6aServiceWireCommand.nodeRequest
          ? decodeNodeRequestHeader(received.parts[0]!)
          : header.command === M6aServiceWireCommand.channelRequest
            ? decodeChannelRequestHeader(received.parts[0]!).correlation
            : undefined;
      if (correlation === undefined) return false;
      const reply = [
        encodeReplyHeader(
          correlation,
          RequestResult.ProtocolError,
          ServiceWireFrameworkErrorCode.requestProtocolError
        )
      ];
      if (received.reply === undefined) return false;
      received.reply(reply);
      return true;
    } catch (error) {
      if (error instanceof ServiceWireProtocolError) return false;
      throw error;
    }
  }

  private processReceived(
    received: ZLinkRawReceivedRecord,
    nowMs: number,
    applicationJobOwner: ApplicationIngressRecordOwner
  ): RawServicePumpResult | Promise<RawServicePumpResult> {
    const peer = this.topology.admittedPeer(received.sourceRid);
    const currentSource =
      peer?.liveness !== undefined && peer.liveness.connectionId === received.routeGeneration;
    if (received.parts.length === 0) return currentSource ? 'protocolError' : 'dropped';
    try {
      const header = decodeHeader(received.parts[0]!);
      if (header.command === M6aServiceWireCommand.hello && this.peerAdmissionSealed?.() === true) {
        return 'dropped';
      }
      if (
        header.command === M6aServiceWireCommand.hello ||
        header.command === M6aServiceWireCommand.admit
      ) {
        return this.finishPendingProtocolReceive(this.processAdmission(received, header, nowMs));
      }
      if (header.command === M6aServiceWireCommand.reject) {
        if (received.parts.length !== 1) return 'protocolError';
        decodeReject(received.parts[0]!);
        return 'infrastructure';
      }
      if (!currentSource) return 'dropped';
      this.liveness.recordReceived(peer.liveness, nowMs);
      if (header.command === M6aServiceWireCommand.update) {
        return this.finishPendingProtocolReceive(this.processAdmission(received, header, nowMs));
      }
      if (
        header.command === M6aServiceWireCommand.livenessProbe ||
        header.command === M6aServiceWireCommand.livenessAck
      ) {
        return this.finishPendingProtocolReceive(this.processLiveness(received, nowMs));
      }
      const stateful =
        this.serviceIngress !== undefined && this.serviceIngressAccepts?.(header.command) !== false
          ? this.serviceIngress({
              command: header.command,
              flags: header.flags,
              sourceRoutingId: received.sourceRid,
              sourceNodeGeneration: peer.descriptor.lifecycleGeneration,
              sourceRoute: received.sourceRoute,
              ...(received.reply === undefined ? {} : { reply: received.reply }),
              ...(received.requestSeq === undefined
                ? {}
                : { requestSequence: received.requestSeq }),
              parts: received.parts,
              applicationJobOwner
            })
          : undefined;
      if (stateful instanceof Promise)
        return this.finishPendingStatefulReceive(stateful, received, header, applicationJobOwner);
      if (stateful !== undefined) return stateful;
      return this.processApplicationReceived(received, header, applicationJobOwner);
    } catch (error) {
      if (!currentSource && error instanceof ServiceWireProtocolError) return 'dropped';
      return this.receiveProtocolFailure(error);
    }
  }

  private finishPendingProtocolReceive(
    pending: Promise<RawServicePumpResult>
  ): Promise<RawServicePumpResult> {
    return pending.catch((error) => this.receiveProtocolFailure(error));
  }

  private finishPendingStatefulReceive(
    pending: Promise<RawServicePumpResult | undefined>,
    received: ZLinkRawReceivedRecord,
    header: ReturnType<typeof decodeHeader>,
    owner: ApplicationIngressRecordOwner
  ): Promise<RawServicePumpResult> {
    return pending.then(
      (result) => result ?? this.processApplicationReceived(received, header, owner),
      (error) => this.receiveProtocolFailure(error)
    );
  }

  private receiveProtocolFailure(error: unknown): RawServicePumpResult {
    if (error instanceof ServiceWireProtocolError) return 'protocolError';
    throw error;
  }

  private async processAdmission(
    received: ZLinkRawReceivedRecord,
    header: ReturnType<typeof decodeHeader>,
    nowMs: number
  ): Promise<RawServicePumpResult> {
    if (received.parts.length !== 1) return 'protocolError';
    const descriptor = decodeRouteMeshAdmission(
      received.parts[0]!,
      header.command,
      received.sourceRid
    );
    const expected = this.expectedPeers.get(received.sourceRid);
    if (
      expected !== undefined &&
      (expected.meshName !== descriptor.meshName ||
        expected.nodeRoutingId !== descriptor.nodeRoutingId)
    ) {
      await this.sendControl(received.sourceRid, [
        encodeReject(enumWireRejectReason('identityMismatch'))
      ]);
      return 'infrastructure';
    }
    const helloKey = controlSendKey(received.sourceRid, M6aServiceWireCommand.hello);
    const exchange = this.controlSends.get(helloKey);
    const result = this.admitPeer(
      descriptor,
      received.routeGeneration,
      nowMs,
      expected,
      header.command,
      received.parts[0]!.subarray(SERVICE_WIRE_PREFIX_SIZE)
    );
    if (result !== 'admitted' && result !== 'notRequired') {
      const detail =
        result === 'invalidDescriptor'
          ? describeInvalidAdmissionDescriptor(
              descriptor,
              expected,
              this.topology.peer(descriptor.nodeRoutingId)?.descriptor
            )
          : undefined;
      console.warn(
        `RouteMesh admission rejected peer '${received.sourceRid}' as ${result}` +
          (detail === undefined ? '.' : ` (${detail}).`)
      );
      await this.sendControl(received.sourceRid, [encodeReject(admissionReason(result))]);
      return 'infrastructure';
    }
    if (result === 'admitted') this.selectBilateralConnection(descriptor);
    if (header.command === M6aServiceWireCommand.hello) {
      // Hello supplies only the sender's descriptor. Even NotRequired
      // peers must receive ours before the intent owner closes transport;
      // submitting a response does not guarantee delivery before close.
      const local =
        exchange?.current?.() === true && exchange.hello !== undefined
          ? decodeRouteMeshAdmission(
              exchange.hello,
              M6aServiceWireCommand.hello,
              this.descriptor.nodeRoutingId
            )
          : this.topology.localDescriptor();
      await this.sendControl(received.sourceRid, [
        encodeRouteMeshAdmission(M6aServiceWireCommand.admit, local)
      ]);
    } else if (header.command === M6aServiceWireCommand.admit) {
      this.controlSends.delete(helloKey);
      this.progressPeerAdmission(nowMs);
      if (result === 'notRequired') {
        this.retireNotRequiredExpectedPeer(received.sourceRid, descriptor.advertisedEndpoint);
      } else if (
        exchange?.hello !== undefined &&
        decodeRouteMeshAdmission(
          exchange.hello,
          M6aServiceWireCommand.hello,
          this.descriptor.nodeRoutingId
        ).descriptorRevision < this.descriptor.descriptorRevision
      ) {
        await this.publishLocalDescriptor([received.sourceRid]);
      }
    }
    return 'infrastructure';
  }

  private async processLiveness(
    received: ZLinkRawReceivedRecord,
    nowMs: number
  ): Promise<RawServicePumpResult> {
    if (received.parts.length !== 1) return 'protocolError';
    const record = livenessCodec.decodeLivenessRecord(received.parts[0]!);
    if (record.command === M6aServiceWireCommand.livenessProbe) {
      const ack = this.liveness.acknowledgeProbe(
        received.sourceRid,
        received.routeGeneration,
        record.probeId
      );
      if (ack === undefined) return 'protocolError';
      const sent = await this.sendControl(received.sourceRid, [
        livenessCodec.encodeLivenessRecord({
          command: M6aServiceWireCommand.livenessAck,
          probeId: record.probeId
        })
      ]);
      if (!sent) return 'dropped';
    } else {
      this.liveness.acknowledge(
        received.sourceRid,
        received.routeGeneration,
        record.probeId,
        nowMs
      );
    }
    return 'infrastructure';
  }

  private processApplicationReceived(
    received: ZLinkRawReceivedRecord,
    header: ReturnType<typeof decodeHeader>,
    applicationJobOwner: ApplicationIngressRecordOwner
  ): RawServicePumpResult | Promise<RawServicePumpResult> {
    try {
      if (
        header.flags === 0 &&
        // A bare infrastructure control is normally exactly one frame, but a
        // relocation Prepare now rides its bound-session journal as
        // node-internal ZLNI frames trailing the canonical command frame
        // (spec 28 §4.4). Gating on the command vocabulary alone — instead
        // of also requiring exactly one part — keeps that multi-frame
        // Prepare from missing this infrastructure-mailbox acceptance and
        // falling through to the two-frame application-envelope check below,
        // which does not recognize command 40 and rejected it as a
        // protocolError (surfaced downstream as `invalid_frame`, stalling
        // the source's Prepare resend loop past the relocation admission
        // budget).
        received.parts.length >= 1 &&
        isBareInfrastructureControl(header.command)
      ) {
        const accepted = this.mailbox.tryEnqueue({
          owner: this.nodeMailboxOwner,
          domain: 'infrastructure',
          parts: received.parts,
          sourceRoutingId: received.sourceRid,
          sourceRoute: received.sourceRoute,
          ...(received.reply === undefined ? {} : { reply: received.reply }),
          ...(received.requestSeq === undefined ? {} : { requestSequence: received.requestSeq })
        });
        return accepted ? 'infrastructure' : 'dropped';
      }
      if (
        header.flags !== 0 ||
        received.parts.length !== 2 ||
        !isApplicationEnvelopeCommand(header.command)
      ) {
        return 'protocolError';
      }
      let owner: string;
      let correlation: bigint | undefined;
      if (header.command === M6aServiceWireCommand.nodeSend) {
        owner = this.nodeMailboxOwner;
      } else if (header.command === M6aServiceWireCommand.nodeRequest) {
        owner = this.nodeMailboxOwner;
        correlation = decodeNodeRequestHeader(received.parts[0]!, header);
      } else if (header.command === M6aServiceWireCommand.channelSend) {
        owner = `channel:${decodeChannelSendHeader(received.parts[0]!, header)}`;
      } else {
        const channel = decodeChannelRequestHeader(received.parts[0]!, header);
        owner = `channel:${channel.channelName}`;
        correlation = channel.correlation;
      }
      const job = applicationJobOwner.acquire('application');
      return job instanceof Promise
        ? this.enqueuePendingOrdinaryRecord(job, received, owner, correlation)
        : this.enqueueOrdinaryRecord(received, owner, correlation, job);
    } catch (error) {
      return this.receiveProtocolFailure(error);
    }
  }

  private enqueuePendingOrdinaryRecord(
    pending: Promise<
      import('../application-jobs/application-ingress-record-owner').ApplicationJobRecordLease
    >,
    received: ZLinkRawReceivedRecord,
    owner: string,
    correlation: bigint | undefined
  ): Promise<RawServicePumpResult> {
    return pending.then((permit) =>
      this.enqueueOrdinaryRecord(received, owner, correlation, permit)
    );
  }

  private enqueueOrdinaryRecord(
    received: ZLinkRawReceivedRecord,
    owner: string,
    correlation: bigint | undefined,
    applicationJob: import('../application-jobs/application-ingress-record-owner').ApplicationJobRecordLease
  ): RawServicePumpResult {
    const accepted = this.mailbox.tryEnqueue({
      owner,
      domain: 'application',
      parts: received.parts,
      sourceRoutingId: received.sourceRid,
      sourceRoute: received.sourceRoute,
      reply: received.reply,
      requestSequence: received.requestSeq,
      correlation,
      applicationJob
    });
    if (accepted) return 'application';
    applicationJob.close();
    if (correlation !== undefined && received.requestSeq !== undefined) {
      this.replyService(
        {
          sourceRoutingId: received.sourceRid,
          sourceRoute: received.sourceRoute,
          requestSequence: received.requestSeq,
          ...(received.reply === undefined ? {} : { reply: received.reply })
        },
        [encodeReplyHeader(correlation, RequestResult.NotConnected, 0)]
      );
      return 'infrastructure';
    }
    return 'dropped';
  }

  expireOperations(nowMs: number, turnDeadlineMs: number): number {
    return this.requestOwner === undefined && nowMs >= this.operations.nextDeadlineMs()
      ? this.operations.expire(nowMs, turnDeadlineMs)
      : 0;
  }

  get pendingOperationCount(): number {
    return this.operations.size;
  }

  cancelPendingOperations(reason: unknown): void {
    this.operations.failAll(reason);
  }

  /**
   * The next management deadline of this node (spec 08 §4.2): request expiry,
   * liveness probe or timeout, or an owned HELLO exchange still awaiting
   * ADMIT, which keeps the existing announce cadence.
   */
  nextMaintenanceDeadlineMs(nowMs: number): number {
    this.progressPeerAdmission(nowMs);
    return Math.min(
      this.requestOwner === undefined ? this.operations.nextDeadlineMs() : Infinity,
      this.liveness.nextDeadlineMs()
    );
  }

  private progressPeerAdmission(nowMs: number): void {
    let pending = false;
    if (!this.closed && this.controlSends.size !== 0) {
      for (const nodeRoutingId of this.selectedRoutes.keys()) {
        const key = controlSendKey(nodeRoutingId, M6aServiceWireCommand.hello);
        const exchange = this.controlSends.get(key);
        // An unsent HELLO intent may resume after the host admission seal opens.
        // Only retransmission of submitted bytes waits for the management cadence.
        if (exchange?.current?.() === true) {
          if (exchange.hello === undefined) this.announcePeer(nodeRoutingId);
          pending = true;
        }
      }
    }
    this.liveness.setAdmissionPending(pending, nowMs);
  }

  tickLiveness(nowMs = performance.now()): ServiceLivenessTick {
    this.progressPeerAdmission(nowMs);
    const admissionDue = this.liveness.tickAdmission(nowMs);
    const result = this.liveness.tick(nowMs);
    this.requireStarted();
    if (!admissionDue && result.probes.length === 0 && result.timedOutNodes.length === 0)
      return result;
    if (admissionDue)
      for (const nodeRoutingId of this.selectedRoutes.keys()) void this.announcePeer(nodeRoutingId);
    for (const probe of result.probes) {
      const parts = [
        livenessCodec.encodeLivenessRecord({
          command: M6aServiceWireCommand.livenessProbe,
          probeId: probe.probeId
        })
      ];
      this.startControlSend(probe.nodeRoutingId, M6aServiceWireCommand.livenessProbe, () =>
        this.submitControl(probe.nodeRoutingId, parts)
      );
    }
    const failures: unknown[] = [];
    for (const nodeRoutingId of result.timedOutNodes) {
      const peer = this.topology.peer(nodeRoutingId);
      if (peer === undefined) continue;
      try {
        this.removePeer(peer, () => {
          try {
            this.requireStarted().disconnectRid!(nodeRoutingId);
          } catch (error) {
            failures.push(error);
          }
        });
      } catch (error) {
        failures.push(error);
      }
    }
    if (failures.length === 1) throw failures[0];
    if (failures.length > 1) throw new AggregateError(failures, 'Liveness peer cleanup failed.');
    return result;
  }

  /**
   * The socket's one route observer (Core ROUTER §10.1). It replaces the
   * observation with Core's selected-route snapshot, ends the admission of
   * every RID whose observed route disappeared or was replaced, and starts the
   * handshake of every newly selected route. Returns the number of changes.
   */
  async observeSelectedRoutes(): Promise<number> {
    const router = this.router;
    if (router === undefined || this.closed) return 0;
    const observed = new Map<string, bigint>();
    for (const route of router.routesSnapshot()) {
      observed.set(route.routingId, route.routeGeneration);
    }
    let changes = 0;
    for (const [nodeRoutingId, generation] of this.selectedRoutes) {
      if (observed.get(nodeRoutingId) === generation) continue;
      this.controlSends.delete(controlSendKey(nodeRoutingId, M6aServiceWireCommand.hello));
      changes++;
      const peer = this.topology.peer(nodeRoutingId);
      if (peer?.liveness?.connectionId === generation) {
        this.removePeer(peer);
      }
    }
    const selected: string[] = [];
    for (const [nodeRoutingId, generation] of observed) {
      if (this.selectedRoutes.get(nodeRoutingId) === generation) continue;
      if (!this.selectedRoutes.has(nodeRoutingId)) changes++;
      selected.push(nodeRoutingId);
    }
    this.selectedRoutes = observed;
    for (const nodeRoutingId of selected) this.notifyPeerRouteChanged(nodeRoutingId);
    // Selected routes begin admission before waiting for the management cadence.
    for (const nodeRoutingId of selected) await this.announcePeer(nodeRoutingId);
    this.progressPeerAdmission(performance.now());
    return changes;
  }

  close(): void {
    const host = this.host;
    if (this.closed && host === undefined) return;
    this.host = undefined;
    try {
      if (!this.closed) {
        this.closed = true;
        this.applicationJobStop.abort(
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RuntimeShutdown,
            'Raw service runtime application job admission stopped.'
          )
        );
        this.receivePermit?.releaseAfterInternalProcessing();
        this.receivePermit?.releaseStorage?.();
        this.receivePermit = undefined;
        for (const send of this.controlSends.values()) delete send.next;
        this.controlSends.clear();
        this.mailbox.close();
        this.releaseOperations();
        this.serviceIngress = undefined;
        const router = this.router;
        if (router !== undefined) {
          this.applicationJobQueue.unregisterReceiveFlowTarget?.(router);
        }
      }
      if (host !== undefined) host.close();
      this.router = undefined;
    } catch (error) {
      this.host = host;
      throw error;
    }
  }

  private requestToTarget(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    channelName?: string,
    onAdmitted?: () => void,
    observer?: RawServiceRequestObserver,
    operationKind?: number,
    requestOperation?: PendingOperation<unknown>
  ): RawServiceObservedRequest {
    const correlation = this.nextCorrelation++;
    const header =
      channelName === undefined
        ? encodeNodeRequestHeader(correlation)
        : encodeChannelRequestHeader(correlation, channelName);
    const encodedPayload = this.applicationFrame(payload);
    let selectedTargetNodeRoutingId = targetNodeRoutingId;
    if (
      selectedTargetNodeRoutingId !== this.descriptor.nodeRoutingId &&
      !this.isPeerRouteReady(selectedTargetNodeRoutingId)
    ) {
      // Channel selection happens before native admission. If that selection
      // becomes stale during a peer drain, choose another ready channel peer
      // while no request has been submitted yet. Reusing the same pending
      // operation and correlation keeps this recovery pre-admission and
      // cannot duplicate an application request.
      if (channelName !== undefined) {
        const excludedTargets = new Set([selectedTargetNodeRoutingId]);
        for (;;) {
          const alternate = this.topology.selectChannel(channelName, (peer) => {
            const candidate = peer.descriptor.nodeRoutingId;
            return !excludedTargets.has(candidate) && this.isPeerRouteReady(candidate);
          });
          if (alternate === undefined) break;
          selectedTargetNodeRoutingId = alternate.descriptor.nodeRoutingId;
          if (this.isPeerRouteReady(selectedTargetNodeRoutingId)) {
            break;
          }
          excludedTargets.add(selectedTargetNodeRoutingId);
        }
      }
    }
    if (selectedTargetNodeRoutingId === this.descriptor.nodeRoutingId) {
      // The local admission path aborts its capacity wait on the terminal
      // Promise, so a local request always keeps the Promise entry.
      const pending =
        requestOperation === undefined
          ? this.operations.register(timeoutMs)
          : (this.registerRequest(
              timeoutMs,
              observer,
              operationKind,
              requestOperation
            ) as PendingOperation<RawServiceRequestResult>);
      const parts = [header, encodedPayload];
      const capacityStop = new AbortController();
      void pending.promise.finally(() => capacityStop.abort()).catch(() => undefined);
      void (async () => {
        const applicationJobOwner = await this.reserveLocalIngress(capacityStop.signal);
        try {
          if (!pending.isPending()) return;
          const applicationJob = await applicationJobOwner.acquire(
            'application',
            capacityStop.signal
          );
          if (!pending.isPending()) {
            applicationJob.close();
            return;
          }
          const accepted = this.mailbox.tryEnqueue({
            owner: `node:${this.descriptor.nodeRoutingId}`,
            domain: 'application',
            parts,
            sourceRoutingId: this.descriptor.nodeRoutingId,
            correlation,
            localReply: (terminalResult, failureCode, reply) =>
              this.completeRequest(
                pending,
                {
                  terminalResult,
                  failureCode,
                  ...(reply === undefined ? {} : { payload: reply })
                },
                requestOperation === undefined ? undefined : observer,
                operationKind
              ),
            applicationJob
          });
          if (!accepted) {
            applicationJob.close();
            this.completeRequest(
              pending,
              {
                terminalResult: RequestResult.NotConnected,
                failureCode: 0
              },
              requestOperation === undefined ? undefined : observer,
              operationKind
            );
          } else {
            onAdmitted?.();
          }
        } finally {
          applicationJobOwner.close();
        }
      })().catch((error) =>
        this.failRequest(
          pending,
          error,
          requestOperation === undefined ? undefined : observer,
          operationKind
        )
      );
      return pending;
    }
    const pending = this.registerRequest(timeoutMs, observer, operationKind, requestOperation);
    const selectedPeer = this.topology.readyPeer(selectedTargetNodeRoutingId);
    if (
      selectedPeer === undefined &&
      !this.expectedPeers.has(selectedTargetNodeRoutingId) &&
      this.topology.peer(selectedTargetNodeRoutingId) === undefined
    ) {
      this.completeRequest(
        pending,
        {
          terminalResult: RequestResult.NotFound,
          failureCode: REQUEST_TARGET_NOT_FOUND_FAILURE_CODE
        },
        requestOperation === undefined ? undefined : observer,
        operationKind
      );
      return pending;
    }
    const parts = [header, encodedPayload];
    const completed = (replyParts: readonly Buffer[]) => {
      try {
        if (replyParts.length < 1 || replyParts.length > 2) {
          throw new ServiceWireProtocolError('Invalid reply parts.');
        }
        const reply = decodeReplyHeader(replyParts[0]!);
        if (reply.correlation !== correlation)
          throw new ServiceWireProtocolError('Reply correlation mismatch.');
        if (reply.tail.byteLength !== 0) {
          throw new ServiceWireProtocolError(
            'Generic node/channel reply carries an operation-specific tail.'
          );
        }
        if (reply.terminalResult === 0 && replyParts.length !== 2) {
          throw new ServiceWireProtocolError('Successful reply omits its payload.');
        }
        if (reply.terminalResult !== 0 && replyParts.length !== 1) {
          throw new ServiceWireProtocolError('Failed reply carries a payload.');
        }
        const result: RawServiceRequestResult = {
          terminalResult: reply.terminalResult,
          failureCode: reply.failureCode
        };
        const value =
          reply.terminalResult === 0
            ? { ...result, payload: decodeApplicationPayloadView(replyParts[1]!) }
            : result;
        if (requestOperation !== undefined && observer !== undefined)
          observer.operationCompleted(pending.id, value, operationKind);
        else pending.reply(value);
      } catch (error) {
        this.failRequest(
          pending,
          error,
          requestOperation === undefined ? undefined : observer,
          operationKind
        );
      }
    };
    const failed = (error: unknown) =>
      this.failRequest(
        pending,
        error,
        requestOperation === undefined ? undefined : observer,
        operationKind
      );
    if (selectedPeer !== undefined) {
      try {
        this.requireStarted().request(
          selectedTargetNodeRoutingId,
          parts,
          remainingRequestTimeout('Request', pending.deadlineMs)!,
          completed,
          failed,
          selectedPeer.liveness?.recordReceived,
          onAdmitted,
          pending
        );
      } catch (error) {
        failed(error);
      }
      return pending;
    }
    admitServiceRequest(
      this,
      selectedTargetNodeRoutingId,
      pending,
      (remaining, finished) => {
        this.requireStarted().request(
          selectedTargetNodeRoutingId,
          parts,
          remaining,
          (reply) => {
            finished();
            completed(reply);
          },
          (error) => {
            finished();
            failed(error);
          },
          this.topology.admittedPeer(selectedTargetNodeRoutingId)?.liveness?.recordReceived,
          onAdmitted,
          pending
        );
      },
      failed
    );
    return pending;
  }

  /**
   * The generic node/channel backend may supply its single owned M6A frame
   * directly. All other callers keep the typed application-payload boundary.
   */
  private applicationFrame(payload: ServiceApplicationPayloadInput): Buffer {
    return Buffer.isBuffer(payload) ? payload : encodeApplicationPayload(payload);
  }

  private applicationPayload(payload: ServiceApplicationPayloadInput): ServiceApplicationPayload {
    return Buffer.isBuffer(payload) ? decodeApplicationPayloadView(payload) : payload;
  }

  private admitPeer(
    descriptor: ServiceNodeDescriptor,
    connectionId: string | bigint,
    nowMs: number,
    expected?: {
      readonly endpoint?: string;
      readonly securityIdentity?: string;
      readonly lifecycleGeneration?: bigint;
    },
    command: number = M6aServiceWireCommand.hello,
    descriptorBytes?: Uint8Array
  ): PeerAdmissionResult {
    const previous = this.topology.admittedPeer(descriptor.nodeRoutingId);
    const topologyConnectionId =
      typeof connectionId === 'bigint' ? routeConnectionId(connectionId) : connectionId;
    const result = this.topology.admit(
      descriptor,
      topologyConnectionId,
      expected,
      () => {
        const admission = this.liveness.admit(descriptor.nodeRoutingId, connectionId, nowMs);
        this.liveness.recordReceived(admission, nowMs);
        return admission;
      },
      command,
      descriptorBytes
    );
    if (result === 'staleDescriptor') {
      throw new ServiceWireProtocolError(
        `RouteMesh peer '${descriptor.nodeRoutingId}' descriptor revision is stale or conflicting.`
      );
    }
    if (result === 'notRequired' && previous !== undefined) {
      this.liveness.disconnect(
        descriptor.nodeRoutingId,
        previous.liveness?.connectionId ?? previous.connectionId
      );
    }
    if (
      result === 'admitted' &&
      (previous?.connectionId !== topologyConnectionId ||
        previous.descriptor.lifecycleGeneration !== descriptor.lifecycleGeneration ||
        previous.remoteAdmitReceived !==
          this.topology.admittedPeer(descriptor.nodeRoutingId)?.remoteAdmitReceived)
    )
      this.notifyPeerRouteChanged(descriptor.nodeRoutingId);
    return result;
  }

  private retireNotRequiredExpectedPeer(nodeRoutingId: string, advertisedEndpoint?: string): void {
    const expected = this.expectedPeers.get(nodeRoutingId);
    const endpoint =
      expected?.endpoint ??
      (advertisedEndpoint !== undefined && this.endpointOnlyPeers.has(advertisedEndpoint)
        ? advertisedEndpoint
        : undefined);
    if (endpoint !== undefined) {
      this.requireStarted().disconnect(endpoint);
      this.endpointOnlyPeers.delete(endpoint);
      this.onPeerNotRequired?.(nodeRoutingId, endpoint);
    }
    this.expectedPeers.delete(nodeRoutingId);
    this.notifyPeerConnectionIntentRemoved(nodeRoutingId);
  }

  private selectBilateralConnection(descriptor: ServiceNodeDescriptor): void {
    const endpoint = descriptor.advertisedEndpoint;
    if (!this.endpointOnlyPeers.has(endpoint)) return;
    if (this.expectedPeers.has(descriptor.nodeRoutingId)) return;
    this.expectedPeers.set(descriptor.nodeRoutingId, {
      meshName: descriptor.meshName,
      nodeRoutingId: descriptor.nodeRoutingId,
      endpoint,
      securityIdentity: descriptor.securityIdentity,
      lifecycleGeneration: descriptor.lifecycleGeneration
    });
    this.endpointOnlyPeers.delete(endpoint);
  }

  private removePeer(peer: AdmittedServicePeer, disconnectPhysical?: () => void): void {
    this.topology.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
    this.liveness.disconnect(
      peer.descriptor.nodeRoutingId,
      peer.liveness?.connectionId ?? peer.connectionId
    );
    try {
      disconnectPhysical?.();
    } finally {
      // The logical admission has ended even when the physical disconnect reports a failure.
      this.onPeerDisconnected?.(
        peer.descriptor.nodeRoutingId,
        peer.descriptor.advertisedEndpoint,
        peer.descriptor.lifecycleGeneration
      );
    }
  }

  private requireStarted(): ZLinkRawRouterPort {
    if (this.router === undefined) throw new Error('Raw service runtime is not started.');
    return this.router;
  }

  private async sendControl(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[]
  ): Promise<boolean> {
    return this.startControlSend(targetNodeRoutingId, decodeHeader(parts[0]!).command, () =>
      this.submitControl(targetNodeRoutingId, parts)
    );
  }

  private submitControl(target: string, parts: readonly Uint8Array[]): boolean | Promise<boolean> {
    let submission;
    try {
      submission = this.requireStarted().submitSend(target, parts);
    } catch (error) {
      return this.sendFailure(error);
    }
    if (submission.result === SubmitResult.Ok) return true;
    return submission.admitted.then(
      () => true,
      (error) => this.sendFailure(error)
    );
  }

  private startControlSend(
    target: string,
    command: number,
    attempt: () => boolean | Promise<boolean>,
    current?: () => boolean,
    hello?: Uint8Array
  ): boolean {
    const key = controlSendKey(target, command);
    if (command === M6aServiceWireCommand.hello)
      this.liveness.setAdmissionPending(true, performance.now());
    const existing = this.controlSends.get(key);
    if (existing?.task !== undefined) {
      if (command !== M6aServiceWireCommand.hello) {
        existing.next = attempt;
        existing.current = current;
      }
      return true;
    }
    const first = attempt();
    if (typeof first === 'boolean') {
      if (hello !== undefined) this.controlSends.set(key, { current, hello });
      return first;
    }
    const owner = this;
    const state: ControlSendState = { current, hello };
    const completeSend = () => {
      if (owner.controlSends.get(key) !== state) return;
      if (hello !== undefined) delete state.task;
      else owner.controlSends.delete(key);
    };
    const task = first.then(function drain(accepted): boolean | Promise<boolean> {
      if (!owner.closed && state.next !== undefined) {
        const next = state.next;
        delete state.next;
        return Promise.resolve(next()).then(drain);
      }
      completeSend();
      return accepted;
    });
    state.task = task;
    this.controlSends.set(key, state);
    task.then(
      () => {},
      (error) => {
        completeSend();
        this.reportInfrastructureFailure(error);
      }
    );
    return true;
  }

  reportInfrastructureFailure(error: unknown): void {
    if (this.infrastructureTaskRunner !== undefined) {
      this.infrastructureTaskRunner.runDetached('RouteMesh infrastructure', async () => {
        throw error;
      });
      return;
    }
    this.infrastructureFailure = {
      error:
        this.infrastructureFailure === undefined
          ? error
          : new AggregateError(
              [this.infrastructureFailure.error, error],
              'RouteMesh infrastructure failed.'
            )
    };
    this.onReceiveReady?.();
  }

  private observeInfrastructureFailure(): void {
    const failure = this.infrastructureFailure;
    this.infrastructureFailure = undefined;
    if (failure !== undefined) throw failure.error;
  }

  private send(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[]
  ): boolean | Promise<boolean> {
    if (!this.isPeerRouteReady(targetNodeRoutingId)) return false;
    try {
      const submission = this.requireStarted().submitSend(targetNodeRoutingId, parts);
      if (submission.result === SubmitResult.Ok) return true;
      return submission.admitted.then(
        () => true,
        (error) => this.sendFailure(error)
      );
    } catch (error) {
      return this.sendFailure(error);
    }
  }

  private sendFailure(error: unknown): false {
    if (isZLinkBackendResultError(error) && error.operation === 'submit') {
      const terminal = submitToRequestResult(error.result, error.phase);
      if (terminal === RequestResult.NotConnected || terminal === RequestResult.NotFound)
        return false;
    }
    throw error;
  }
}

function channelSendSubmitResult(accepted: boolean): SubmitResult {
  return accepted ? SubmitResult.Ok : SubmitResult.NotConnected;
}

function controlSendKey(target: string, command: number): string {
  return `${command}:${target}`;
}

function routeConnectionId(generation: bigint): string {
  return `route:${generation}`;
}

/**
 * Relocation and bound-Session controls are service-wire records themselves,
 * not application NodeSend(16) envelopes. Keep this list closed: a bare
 * record outside the frozen control vocabulary remains a protocol error.
 */
const BARE_INFRASTRUCTURE_CONTROLS: ReadonlySet<number> = new Set<number>([
  ServiceWireCommand.relocationReady,
  ServiceWireCommand.relocationData,
  ServiceWireCommand.replyRelay,
  ServiceWireCommand.relocationCutover,
  ServiceWireCommand.relocationPrepare,
  ServiceWireCommand.sessionRelocationSeal,
  ServiceWireCommand.sessionRelocationSealed,
  ServiceWireCommand.sessionRelocationRoute,
  ServiceWireCommand.replyRelayAck,
  ServiceWireCommand.relocationState,
  ServiceWireCommand.relocationFailed
]);

function isBareInfrastructureControl(command: number): boolean {
  return BARE_INFRASTRUCTURE_CONTROLS.has(command);
}

function isApplicationEnvelopeCommand(command: number): boolean {
  return (
    command === M6aServiceWireCommand.nodeSend ||
    command === M6aServiceWireCommand.nodeRequest ||
    command === M6aServiceWireCommand.channelSend ||
    command === M6aServiceWireCommand.channelRequest
  );
}

function protocolCommand(parts: readonly Uint8Array[]): { readonly command?: number } {
  if (parts.length === 0) return {};
  try {
    return { command: decodeHeader(parts[0]!).command };
  } catch {
    return {};
  }
}

function isAlreadyDisconnectedError(error: unknown): boolean {
  if (typeof error !== 'object' || error === null || !('nativeErrno' in error)) {
    return false;
  }
  return (error as { readonly nativeErrno?: unknown }).nativeErrno === nativeErrnoValues.ENOENT;
}

function describeInvalidAdmissionDescriptor(
  descriptor: ServiceNodeDescriptor,
  expected: ServicePeerAdmissionExpectation | undefined,
  current: ServiceNodeDescriptor | undefined
): string {
  try {
    validateDescriptor(descriptor);
  } catch (error) {
    return `descriptor validation: ${error instanceof Error ? error.message : String(error)}`;
  }
  if (expected?.endpoint !== undefined && descriptor.advertisedEndpoint !== expected.endpoint) {
    return `advertisedEndpoint expected='${expected.endpoint}' actual='${descriptor.advertisedEndpoint}'`;
  }
  if (
    expected?.securityIdentity !== undefined &&
    descriptor.securityIdentity !== expected.securityIdentity
  ) {
    return `securityIdentity expected='${expected.securityIdentity}' actual='${descriptor.securityIdentity}'`;
  }
  if (
    expected?.lifecycleGeneration !== undefined &&
    descriptor.lifecycleGeneration !== expected.lifecycleGeneration
  ) {
    return `lifecycleGeneration expected=${expected.lifecycleGeneration} actual=${descriptor.lifecycleGeneration}`;
  }
  if (
    current !== undefined &&
    current.lifecycleGeneration === descriptor.lifecycleGeneration &&
    descriptor.descriptorRevision > current.descriptorRevision
  ) {
    const immutableField = immutableDescriptorMismatch(current, descriptor);
    if (immutableField !== undefined) return immutableField;
  }
  return 'self routing id or immutable descriptor field';
}

function immutableDescriptorMismatch(
  current: ServiceNodeDescriptor,
  descriptor: ServiceNodeDescriptor
): string | undefined {
  const fields: ReadonlyArray<keyof ServiceNodeDescriptor> = [
    'advertisedEndpoint',
    'securityIdentity',
    'applicationVersion',
    'objectRole',
    'activeCapacityLimit',
    'pendingCapacityLimit'
  ];
  for (const field of fields) {
    if (current[field] !== descriptor[field]) {
      return `${field} changed within lifecycle expected=${String(current[field])} actual=${String(descriptor[field])}`;
    }
  }
  if (
    JSON.stringify(current.protocolCapabilities) !== JSON.stringify(descriptor.protocolCapabilities)
  ) {
    return `protocolCapabilities changed within lifecycle expected=${JSON.stringify(current.protocolCapabilities)} actual=${JSON.stringify(descriptor.protocolCapabilities)}`;
  }
  if (
    JSON.stringify(current.channels.map((channel) => channel.name)) !==
    JSON.stringify(descriptor.channels.map((channel) => channel.name))
  ) {
    return 'channels changed within lifecycle';
  }
  return undefined;
}

function admissionReason(result: Exclude<PeerAdmissionResult, 'admitted' | 'notRequired'>): number {
  switch (result) {
    case 'meshMismatch':
      return enumWireRejectReason('topologyMismatch');
    case 'staleDescriptor':
      return enumWireRejectReason('descriptorRevisionStale');
    case 'invalidDescriptor':
      return enumWireRejectReason('invalidDescriptor');
  }
}
