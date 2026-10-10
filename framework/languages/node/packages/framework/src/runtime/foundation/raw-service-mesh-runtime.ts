import { constants as osConstants } from 'node:os';
import { ApplicationIngressRecordOwner } from '../application-jobs/application-ingress-record-owner';
import type {
  ApplicationJobPermitPort,
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
import { OperationRegistry, type PendingOperation } from './operation-registry';
import { ServiceLivenessRegistry, type ServiceLivenessTick } from './service-liveness-registry';
import { ServiceMailbox, type ServiceMailboxRecord } from './service-mailbox';
import {
  sameServiceNodeDescriptor,
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
  readonly command: number;
  readonly flags: number;
  readonly sourceRoutingId: string;
  readonly sourceNodeGeneration?: bigint;
  readonly sourceRoute?: Uint8Array;
  readonly requestSequence?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];
  readonly applicationJobOwner?: ApplicationIngressRecordOwner;
}

export type RawServiceIngressHandler = (
  record: RawServiceIngressRecord
) => RawServicePumpResult | undefined | Promise<RawServicePumpResult | undefined>;

export interface RawServiceMeshRuntimeOptions {
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
const RAW_MESH_RECEIVE_BYTE_BUDGET = 4 * 1024 * 1024;
export const RAW_MESH_RECEIVE_TIME_BUDGET_MS = 2;

const REQUEST_TARGET_NOT_FOUND_FAILURE_CODE = ServiceWireFrameworkErrorCode.requestTargetNotFound;

const livenessCodec = createServiceWireCodec({
  magic: SERVICE_WIRE_MAGIC,
  major: SERVICE_WIRE_MAJOR,
  commands: M6aServiceWireCommand
});

/**
 * RouteMesh M6A runtime built only on the public raw binding package.
 * It owns protocol, admission, mailbox, request and liveness state.
 */
export class RawServiceMeshRuntime {
  readonly topology: ServiceTopologyRegistry;
  readonly mailbox: ServiceMailbox;
  readonly liveness: ServiceLivenessRegistry;

  private readonly operations: OperationRegistry<RawServiceRequestResult>;
  private readonly expectedPeers = new Map<
    string,
    {
      readonly meshName: string;
      readonly nodeRoutingId: string;
      readonly endpoint?: string;
      readonly securityIdentity?: string;
      readonly lifecycleGeneration?: bigint;
      helloSubmittedGeneration?: bigint;
    }
  >();
  private readonly endpointOnlyPeers = new Set<string>();
  private readonly peerConnectionIntentRemoved = new Set<(nodeRoutingId: string) => void>();
  /**
   * The last observed Core selected route of each RID (Core ROUTER §10.1):
   * RID -> route generation. Only `observeSelectedRoutes` replaces it.
   */
  private selectedRoutes = new Map<string, bigint>();
  private readonly bindingPort: ZLinkRawBindingPort;
  private readonly applicationJobQueue: ApplicationJobQueuePort;
  private readonly peerAdmissionSealed?: () => boolean;
  private readonly applicationJobStop = new AbortController();
  private receiveAcquisition?: Promise<ApplicationJobPermitPort>;
  private receivePermit?: ApplicationJobPermitPort;
  private readonly controlSends = new Map<
    string,
    {
      readonly task: Promise<boolean>;
      next?: () => boolean | Promise<boolean>;
      current?: () => boolean;
    }
  >();
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
  private closed = false;

  constructor(options: RawServiceMeshRuntimeOptions) {
    this.onReceiveReady = options.onReceiveReady;
    this.infrastructureTaskRunner = options.infrastructureTaskRunner;
    this.operations = new OperationRegistry(undefined, options.onPendingOperationsChanged);
    this.descriptor = options.descriptor;
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
        router.disconnect(endpoint);
      }
    } catch (error) {
      if (!isAlreadyDisconnectedError(error)) throw error;
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

  private notifyPeerConnectionIntentRemoved(nodeRoutingId: string): void {
    // Only the logical owner's removal publishes lifecycle termination.
    // A physical disconnect alone retains the connection expectation.
    if (this.expectedPeers.has(nodeRoutingId) || this.topology.peer(nodeRoutingId) !== undefined)
      return;
    for (const listener of this.peerConnectionIntentRemoved) listener(nodeRoutingId);
  }

  announcePeer(nodeRoutingId: string): Promise<boolean> {
    const expected = this.expectedPeers.get(nodeRoutingId);
    const generation = this.selectedRoutes.get(nodeRoutingId);
    if (
      expected === undefined ||
      generation === undefined ||
      expected.helloSubmittedGeneration === generation ||
      this.peerAdmissionSealed?.() === true
    )
      return Promise.resolve(false);
    const current = () =>
      this.expectedPeers.get(nodeRoutingId) === expected &&
      this.selectedRoutes.get(nodeRoutingId) === generation;
    return Promise.resolve(
      this.startControlSend(
        nodeRoutingId,
        M6aServiceWireCommand.hello,
        () =>
          this.submitControl(
            nodeRoutingId,
            [
              encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())
            ],
            () => {
              if (current()) expected.helloSubmittedGeneration = generation;
            }
          ),
        current
      )
    );
  }

  isPeerRouteReady(nodeRoutingId: string, lifecycleGeneration?: bigint): boolean {
    // Admission is bound to the observed selected route: the route observer
    // removes the peer when that route ends or is replaced.
    const peer = this.topology.peer(nodeRoutingId);
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
    const update = encodeRouteMeshAdmission(
      M6aServiceWireCommand.update,
      this.topology.localDescriptor()
    );
    for (const peer of this.topology.peers()) {
      await this.send(peer.descriptor.nodeRoutingId, [update]);
    }
  }

  replaceDiscoveredNotRequired(descriptors: readonly ServiceNodeDescriptor[]): void {
    this.topology.replaceDiscoveredNotRequired(descriptors);
  }

  isObjectClientNodeDirectTarget(nodeRoutingId: string): boolean {
    const descriptor =
      nodeRoutingId === this.descriptor.nodeRoutingId
        ? this.topology.localDescriptor()
        : this.topology.knownDescriptor(nodeRoutingId);
    return descriptor?.objectRole === 'client';
  }

  async sendToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput
  ): Promise<boolean> {
    return this.send(targetNodeRoutingId, [encodeNodeSendHeader(), this.applicationFrame(payload)]);
  }

  async sendToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput
  ): Promise<SubmitResult> {
    const selection = this.selectChannelTarget(channelName);
    if (selection.kind !== 'selected') return selection.submitResult;
    const selected = selection.peer;
    const applicationFrame = this.applicationFrame(payload);
    return (await this.send(selected.descriptor.nodeRoutingId, [
      encodeChannelSendHeader(channelName),
      applicationFrame
    ]))
      ? SubmitResult.Ok
      : SubmitResult.NotConnected;
  }

  requestToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number
  ): PendingOperation<RawServiceRequestResult> {
    return this.requestToTarget(targetNodeRoutingId, payload, timeoutMs);
  }

  requestToChannel(
    channelName: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number
  ): PendingOperation<RawServiceRequestResult> {
    const selection = this.selectChannelTarget(channelName);
    if (selection.kind !== 'selected') {
      const pending = this.operations.register(timeoutMs);
      this.operations.complete(pending.id, selection.requestResult);
      return pending;
    }
    return this.requestToTarget(
      selection.peer.descriptor.nodeRoutingId,
      payload,
      timeoutMs,
      channelName
    );
  }

  private selectChannelTarget(channelName: string): RawServiceChannelTargetSelection {
    const selected = this.topology.selectChannel(channelName, (peer) =>
      this.isPeerRouteReady(peer.descriptor.nodeRoutingId)
    );
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

  setServiceIngress(handler: RawServiceIngressHandler): void {
    if (this.serviceIngress !== undefined && this.serviceIngress !== handler) {
      throw new Error('Raw service ingress is already registered.');
    }
    this.serviceIngress = handler;
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
    return this.send(targetNodeRoutingId, parts);
  }

  requestService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]> {
    const admission = this.topology.peer(targetNodeRoutingId)?.liveness;
    return this.requireStarted().request(
      targetNodeRoutingId,
      parts,
      timeoutMs,
      admission?.recordReceived
    );
  }

  replyService(
    record: Pick<
      RawServiceIngressRecord,
      'sourceRoutingId' | 'sourceRoute' | 'requestSequence' | 'reply'
    >,
    parts: readonly Uint8Array[]
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
    request.reply([
      encodeReplyHeader(request.correlation, terminalResult, failureCode),
      ...(terminalResult === 0 ? [this.applicationFrame(payload)] : [])
    ]);
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

  setReadableHandler(handler: (receiveReady: boolean, routeReady: boolean) => void): void {
    this.requireStarted().setReadableHandler(handler);
  }

  /** Returns whether a receive budget ended before no-data; idle ticks only maintain peers. */
  async pumpBatch(receiveReady = true, routeReady = false): Promise<boolean> {
    this.observeInfrastructureFailure();
    if (routeReady) await this.observeSelectedRoutes();
    const startedAtMs = performance.now();
    let messages = 0;
    let bytes = 0;
    const observe: RawServicePumpObserver = (_source, byteCount) => {
      bytes += byteCount;
    };
    while (receiveReady && messages < RAW_MESH_RECEIVE_RECORD_BUDGET && !this.closed) {
      const result = await this.receiveOne(performance.now(), observe);
      if (result === 'noData') {
        receiveReady = false;
        break;
      }
      messages += 1;
      // Core owns the per-peer fair-queue cursor. The Framework limit bounds
      // the entire round, so changing peers does not renew its byte/count budget.
      if (
        bytes >= RAW_MESH_RECEIVE_BYTE_BUDGET ||
        performance.now() - startedAtMs >= RAW_MESH_RECEIVE_TIME_BUDGET_MS
      )
        break;
    }
    if (!this.closed) {
      for (const nodeRoutingId of this.expectedPeers.keys()) {
        if (this.topology.peer(nodeRoutingId) === undefined) await this.announcePeer(nodeRoutingId);
      }
      this.tickLiveness();
    }
    return receiveReady && messages > 0;
  }

  private async receiveOne(
    nowMs: number,
    observe?: RawServicePumpObserver
  ): Promise<RawServicePumpResult> {
    const router = this.requireStarted();
    this.observeInfrastructureFailure();
    if (this.receivePermit === undefined && this.receiveAcquisition === undefined) {
      let resolveAcquisition!: (permit: ApplicationJobPermitPort) => void;
      let rejectAcquisition!: (error: unknown) => void;
      const acquisition = new Promise<ApplicationJobPermitPort>((resolve, reject) => {
        resolveAcquisition = resolve;
        rejectAcquisition = reject;
      });
      const permit = this.applicationJobQueue.acquireOrResume(
        (permit) => {
          this.receiveAcquisition = undefined;
          if (this.closed) permit.releaseAfterInternalProcessing();
          else {
            this.receivePermit = permit;
            this.onReceiveReady?.();
          }
          resolveAcquisition(permit);
        },
        rejectAcquisition,
        this.applicationJobStop.signal,
        'remote'
      );
      if (permit !== undefined) this.receivePermit = permit;
      else {
        this.receiveAcquisition = acquisition;
        acquisition.catch((error) => {
          this.receiveAcquisition = undefined;
          if (!this.applicationJobStop.signal.aborted) this.reportInfrastructureFailure(error);
        });
      }
    }
    const permit = this.receivePermit;
    this.receivePermit = undefined;
    if (permit === undefined) return 'noData';
    if (this.closed) {
      permit.releaseAfterInternalProcessing();
      return 'noData';
    }
    let received: ZLinkRawReceivedRecord | undefined;
    try {
      received = router.receive(true);
    } catch (error) {
      permit.releaseAfterInternalProcessing();
      throw error;
    }
    if (received === undefined) {
      permit.releaseAfterInternalProcessing();
      return 'noData';
    }
    const applicationJobOwner = ApplicationIngressRecordOwner.create(
      this.applicationJobQueue,
      permit,
      received
    );
    try {
      const result = await this.processReceived(received, nowMs, applicationJobOwner);
      if (result === 'protocolError') {
        this.reportProtocolError(received);
      }
      observe?.(
        received.sourceRid,
        received.parts.reduce((sum, part) => sum + part.byteLength, 0)
      );
      return result;
    } finally {
      applicationJobOwner.close();
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

  private async processReceived(
    received: ZLinkRawReceivedRecord,
    nowMs: number,
    applicationJobOwner: ApplicationIngressRecordOwner
  ): Promise<RawServicePumpResult> {
    if (
      received.parts.length === 0 ||
      (received.parts.length === 1 && received.parts[0]!.byteLength === 0)
    ) {
      if (this.peerAdmissionSealed?.() === true) return 'dropped';
      return (await this.sendControl(received.sourceRid, [
        encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())
      ]))
        ? 'infrastructure'
        : 'dropped';
    }
    try {
      const header = decodeHeader(received.parts[0]!);
      if (header.command === M6aServiceWireCommand.hello && this.peerAdmissionSealed?.() === true) {
        return 'dropped';
      }
      if (
        header.command === M6aServiceWireCommand.hello ||
        header.command === M6aServiceWireCommand.admit ||
        header.command === M6aServiceWireCommand.update
      ) {
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
        const result = this.admitPeer(descriptor, received.routeGeneration, nowMs, expected);
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
          await this.sendControl(received.sourceRid, [
            encodeRouteMeshAdmission(M6aServiceWireCommand.admit, this.topology.localDescriptor())
          ]);
        } else if (result === 'notRequired') {
          this.retireNotRequiredExpectedPeer(received.sourceRid, descriptor.advertisedEndpoint);
        }
        return 'infrastructure';
      }
      if (header.command === M6aServiceWireCommand.reject) {
        if (received.parts.length !== 1) return 'protocolError';
        decodeReject(received.parts[0]!);
        return 'infrastructure';
      }
      const peer = this.topology.peer(received.sourceRid);
      if (peer === undefined) return 'protocolError';
      if (peer.liveness !== undefined && peer.liveness.connectionId === received.routeGeneration)
        this.liveness.recordReceived(peer.liveness, nowMs);
      if (
        header.command === M6aServiceWireCommand.livenessProbe ||
        header.command === M6aServiceWireCommand.livenessAck
      ) {
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
      const stateful = await this.serviceIngress?.({
        command: header.command,
        flags: header.flags,
        sourceRoutingId: received.sourceRid,
        sourceNodeGeneration: peer.descriptor.lifecycleGeneration,
        sourceRoute: received.sourceRoute,
        ...(received.reply === undefined ? {} : { reply: received.reply }),
        ...(received.requestSeq === undefined ? {} : { requestSequence: received.requestSeq }),
        parts: received.parts,
        applicationJobOwner
      });
      if (stateful !== undefined) return stateful;
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
          owner: `node:${this.descriptor.nodeRoutingId}`,
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
        ![
          M6aServiceWireCommand.nodeSend,
          M6aServiceWireCommand.nodeRequest,
          M6aServiceWireCommand.channelSend,
          M6aServiceWireCommand.channelRequest
        ].includes(header.command as never)
      ) {
        return 'protocolError';
      }
      let owner: string;
      let correlation: bigint | undefined;
      if (header.command === M6aServiceWireCommand.nodeSend) {
        owner = `node:${this.descriptor.nodeRoutingId}`;
      } else if (header.command === M6aServiceWireCommand.nodeRequest) {
        owner = `node:${this.descriptor.nodeRoutingId}`;
        correlation = decodeNodeRequestHeader(received.parts[0]!);
      } else if (header.command === M6aServiceWireCommand.channelSend) {
        owner = `channel:${decodeChannelSendHeader(received.parts[0]!)}`;
      } else {
        const channel = decodeChannelRequestHeader(received.parts[0]!);
        owner = `channel:${channel.channelName}`;
        correlation = channel.correlation;
      }
      const applicationJob = await applicationJobOwner.acquire('application');
      const accepted = this.mailbox.tryEnqueue({
        owner,
        domain: 'application',
        parts: received.parts,
        sourceRoutingId: received.sourceRid,
        sourceRoute: received.sourceRoute,
        ...(received.reply === undefined ? {} : { reply: received.reply }),
        requestSequence: received.requestSeq,
        ...(correlation === undefined ? {} : { correlation }),
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
    } catch (error) {
      if (error instanceof ServiceWireProtocolError) return 'protocolError';
      throw error;
    }
  }

  expireOperations(nowMs: number, turnDeadlineMs: number): number {
    return this.operations.expire(nowMs, turnDeadlineMs);
  }

  get pendingOperationCount(): number {
    return this.operations.size;
  }

  tickLiveness(nowMs = performance.now()): ServiceLivenessTick {
    const result = this.liveness.tick(nowMs);
    this.requireStarted();
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
    for (const nodeRoutingId of result.timedOutNodes) {
      const peer = this.topology.peer(nodeRoutingId);
      if (peer === undefined) continue;
      // Liveness timeout is a semantic peer removal of the admitted route.
      this.removePeer(peer);
    }
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
      changes++;
      const expected = this.expectedPeers.get(nodeRoutingId);
      if (expected !== undefined) delete expected.helloSubmittedGeneration;
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
    // A new selected route admits only through a new handshake.
    for (const nodeRoutingId of selected) await this.announcePeer(nodeRoutingId);
    return changes;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.applicationJobStop.abort(
      new Error('Raw service runtime application job admission stopped.')
    );
    this.receivePermit?.releaseAfterInternalProcessing();
    this.receivePermit = undefined;
    for (const send of this.controlSends.values()) delete send.next;
    this.mailbox.close();
    this.operations.close('Raw service runtime closed.');
    this.serviceIngress = undefined;
    const router = this.router;
    if (router !== undefined) {
      this.applicationJobQueue.unregisterReceiveFlowTarget?.(router);
    }
    const host = this.host;
    if (host !== undefined) host.close();
    this.router = undefined;
    this.host = undefined;
  }

  private requestToTarget(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayloadInput,
    timeoutMs: number,
    channelName?: string
  ): PendingOperation<RawServiceRequestResult> {
    const correlation = this.nextCorrelation++;
    const header =
      channelName === undefined
        ? encodeNodeRequestHeader(correlation)
        : encodeChannelRequestHeader(correlation, channelName);
    const encodedPayload = this.applicationFrame(payload);
    const pending = this.operations.register(timeoutMs);
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
      const parts = [header, encodedPayload];
      const capacityStop = new AbortController();
      void pending.promise.finally(() => capacityStop.abort()).catch(() => undefined);
      void (async () => {
        const applicationJobOwner = await this.reserveLocalIngress(capacityStop.signal);
        try {
          if (!this.operations.isPending(pending.id)) return;
          const applicationJob = await applicationJobOwner.acquire(
            'application',
            capacityStop.signal
          );
          if (!this.operations.isPending(pending.id)) {
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
              this.operations.complete(pending.id, {
                terminalResult,
                failureCode,
                ...(reply === undefined ? {} : { payload: reply })
              }),
            applicationJob
          });
          if (!accepted) {
            applicationJob.close();
            this.operations.complete(pending.id, {
              terminalResult: RequestResult.NotConnected,
              failureCode: 0
            });
          }
        } finally {
          applicationJobOwner.close();
        }
      })().catch((error) => this.operations.fail(pending.id, error));
      return pending;
    }
    const selectedPeer = this.topology.peer(selectedTargetNodeRoutingId);
    if (selectedPeer === undefined) {
      // Logical target selection is the requester's decision: an RID that is
      // not an admitted peer is not a target. Whether an admitted target's
      // route can carry the request is Core's REQUEST result.
      this.operations.complete(pending.id, {
        terminalResult: RequestResult.NotFound,
        failureCode: REQUEST_TARGET_NOT_FOUND_FAILURE_CODE
      });
      return pending;
    }
    const parts = [header, encodedPayload];
    let request: Promise<readonly Uint8Array[]>;
    try {
      request = this.requireStarted().request(
        selectedTargetNodeRoutingId,
        parts,
        timeoutMs,
        selectedPeer.liveness?.recordReceived
      );
    } catch (error) {
      this.operations.fail(pending.id, error);
      return pending;
    }
    void request.then(
      (replyParts) => {
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
          this.operations.complete(
            pending.id,
            reply.terminalResult === 0
              ? { ...result, payload: decodeApplicationPayloadView(replyParts[1]!) }
              : result
          );
        } catch (error) {
          this.operations.fail(pending.id, error);
        }
      },
      (error) => {
        this.operations.fail(pending.id, error);
      }
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
    }
  ): PeerAdmissionResult {
    const previous = this.topology.peer(descriptor.nodeRoutingId);
    if (
      previous !== undefined &&
      previous.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration &&
      (descriptor.descriptorRevision < previous.descriptor.descriptorRevision ||
        (descriptor.descriptorRevision === previous.descriptor.descriptorRevision &&
          !sameServiceNodeDescriptor(previous.descriptor, descriptor)))
    ) {
      throw new ServiceWireProtocolError(
        `RouteMesh peer '${descriptor.nodeRoutingId}' descriptor revision is stale or conflicting.`
      );
    }
    const topologyConnectionId =
      typeof connectionId === 'bigint' ? `route:${connectionId}` : connectionId;
    const result = this.topology.admit(descriptor, topologyConnectionId, expected, () => {
      const admission = this.liveness.admit(descriptor.nodeRoutingId, connectionId, nowMs);
      this.liveness.recordReceived(admission, nowMs);
      return admission;
    });
    if (result === 'notRequired' && previous !== undefined) {
      this.liveness.disconnect(
        descriptor.nodeRoutingId,
        previous.liveness?.connectionId ?? previous.connectionId
      );
    }
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

  private removePeer(peer: AdmittedServicePeer): void {
    this.topology.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
    this.liveness.disconnect(
      peer.descriptor.nodeRoutingId,
      peer.liveness?.connectionId ?? peer.connectionId
    );
    this.onPeerDisconnected?.(
      peer.descriptor.nodeRoutingId,
      peer.descriptor.advertisedEndpoint,
      peer.descriptor.lifecycleGeneration
    );
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

  private submitControl(
    target: string,
    parts: readonly Uint8Array[],
    accepted?: () => void
  ): boolean | Promise<boolean> {
    let submission;
    try {
      submission = this.requireStarted().submitSend(target, parts);
    } catch (error) {
      return this.sendFailure(error);
    }
    if (submission.result === SubmitResult.Ok) {
      accepted?.();
      return true;
    }
    return submission.admitted.then(
      () => {
        accepted?.();
        return true;
      },
      (error) => this.sendFailure(error)
    );
  }

  private startControlSend(
    target: string,
    command: number,
    attempt: () => boolean | Promise<boolean>,
    current?: () => boolean
  ): boolean {
    const key = JSON.stringify([target, command]);
    const existing = this.controlSends.get(key);
    if (
      existing !== undefined &&
      (command !== M6aServiceWireCommand.hello || existing.current?.() === true)
    ) {
      if (command !== M6aServiceWireCommand.hello) {
        existing.next = attempt;
        existing.current = current;
      }
      return true;
    }
    const first = attempt();
    if (typeof first === 'boolean') return first;
    const owner = this;
    const state: {
      task: Promise<boolean>;
      next?: () => boolean | Promise<boolean>;
      current?: () => boolean;
    } = {
      current,
      task: first.then(function drain(accepted): boolean | Promise<boolean> {
        if (!owner.closed && state.next !== undefined) {
          const next = state.next;
          delete state.next;
          return Promise.resolve(next()).then(drain);
        }
        if (owner.controlSends.get(key) === state) owner.controlSends.delete(key);
        return accepted;
      })
    };
    this.controlSends.set(key, state);
    state.task.then(
      () => {},
      (error) => {
        if (this.controlSends.get(key) === state) this.controlSends.delete(key);
        if (this.infrastructureTaskRunner === undefined) this.reportInfrastructureFailure(error);
      }
    );
    this.infrastructureTaskRunner?.runDetached('RouteMesh control SEND', async () => {
      await state.task;
    });
    return true;
  }

  private reportInfrastructureFailure(error: unknown): void {
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

  private send(targetNodeRoutingId: string, parts: readonly Uint8Array[]): Promise<boolean> {
    return this.requireStarted()
      .send(targetNodeRoutingId, parts)
      .then(
        () => {
          return true;
        },
        (error) => {
          return this.sendFailure(error);
        }
      );
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

/**
 * Relocation and bound-Session controls are service-wire records themselves,
 * not application NodeSend(16) envelopes. Keep this list closed: a bare
 * record outside the frozen control vocabulary remains a protocol error.
 */
function isBareInfrastructureControl(command: number): boolean {
  return [
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
  ].includes(command as never);
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
