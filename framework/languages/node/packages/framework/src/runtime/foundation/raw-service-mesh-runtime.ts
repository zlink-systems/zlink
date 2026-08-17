import { randomUUID } from 'node:crypto';
import type {
  ZLinkRawBindingPort,
  ZLinkRawHostPort,
  ZLinkRawMonitorRecord,
  ZLinkRawMonitorPort,
  ZLinkRawReceivedRecord,
  ZLinkRawRouterPort
} from '../backend/raw-binding-port';
import { RequestResult } from '../backend/runtime-values';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import {
  ApplicationIngressRecordOwner
} from '../application-jobs/application-ingress-record-owner';
import { OperationRegistry, type PendingOperation } from './operation-registry';
import { ServiceLivenessRegistry, type ServiceLivenessTick } from './service-liveness-registry';
import { ServiceMailbox, type ServiceMailboxLimits, type ServiceMailboxRecord } from './service-mailbox';
import {
  ServiceTopologyRegistry,
  sameServiceNodeDescriptor,
  type AdmittedServicePeer,
  type PeerAdmissionResult,
  type ServiceNodeDescriptor
} from './service-topology-registry';
import {
  decodeApplicationPayload,
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
  type ServiceApplicationPayload,
  ServiceWireProtocolError
} from './service-wire-m6a-codec';
import { createServiceWireCodec } from './service-wire-codec';
import { ServiceWireFrameworkErrorCode } from './service-wire-constants.generated';

export type RawServicePumpResult =
  | 'noData'
  | 'infrastructure'
  | 'application'
  | 'dropped'
  | 'protocolError';

export type RawServicePumpObserver = (
  sourceRoutingId: string,
  byteCount: number
) => void;

export interface RawServiceRequestResult {
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly payload?: ServiceApplicationPayload;
}

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
  readonly descriptor: ServiceNodeDescriptor;
  readonly mailbox?: Partial<ServiceMailboxLimits>;
  readonly probeIntervalMs?: number;
  readonly peerTimeoutMs?: number;
  readonly bindingPort: ZLinkRawBindingPort;
  readonly applicationJobQueue?: ApplicationJobQueuePort;
  readonly onMailboxReady?: (domain: 'application' | 'infrastructure') => void;
  readonly onPeerNotRequired?: (
    nodeRoutingId: string,
    endpoint: string
  ) => void;
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

const MONITOR_DISCONNECTED = 0x0200;
const MONITOR_CONNECTION_READY = 0x1000;
const MONITOR_CONNECTION_READY_EDGE = 1;
// Framework error code 13 (RequestTargetNotFound) is encoded as 14 on a
// RequestResult.NotFound reply. Boundary transport results keep failureCode 0.
const REQUEST_TARGET_NOT_FOUND_FAILURE_CODE = 14;
type PhysicalConnectionDirection = 'inbound' | 'outbound' | 'unknown';

interface PhysicalConnectionCandidate {
  readonly connectionId: string;
  readonly direction: PhysicalConnectionDirection;
  readonly discriminator: string;
  readonly localAddress: string;
  readonly remoteAddress: string;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
}

const livenessCodec = createServiceWireCodec({
  magic: [0x5a, 0x4d],
  major: 1,
  commands: M6aServiceWireCommand
});

const MAX_PENDING_MONITOR_EVENTS = 8192;
const MAX_UNRESOLVED_CONNECTION_CANDIDATES = 4096;

/**
 * RouteMesh M6A runtime built only on the public raw binding package.
 * It owns protocol, admission, mailbox, request and liveness state.
 */
export class RawServiceMeshRuntime {
  readonly topology: ServiceTopologyRegistry;
  readonly mailbox: ServiceMailbox;
  readonly liveness: ServiceLivenessRegistry;

  private readonly operations = new OperationRegistry<RawServiceRequestResult>();
  private readonly expectedPeers = new Map<string, {
    readonly meshName: string;
    readonly nodeRoutingId: string;
    readonly endpoint?: string;
    readonly securityIdentity?: string;
    readonly lifecycleGeneration?: bigint;
  }>();
  private readonly endpointOnlyPeers = new Set<string>();
  private readonly connectionCandidates = new Map<
    string,
    Map<string, PhysicalConnectionCandidate>
  >();
  private readonly unresolvedConnectionCandidates: PhysicalConnectionCandidate[] = [];
  private readonly connectionIds = new Map<string, string>();
  /**
   * The monitor callback updates this fence before the event reaches the
   * normal runtime drain. `null` means that the last observed pair is no
   * longer usable by the application route.
   */
  private readonly monitorConnectionStates = new Map<string, string | null>();
  private monitorEvents: ZLinkRawMonitorRecord[] = [];
  private monitorDrainBuffer: ZLinkRawMonitorRecord[] = [];
  private readonly bindingPort: ZLinkRawBindingPort;
  private readonly applicationJobQueue: ApplicationJobQueuePort;
  private readonly applicationJobStop = new AbortController();
  private readonly onPeerNotRequired?: RawServiceMeshRuntimeOptions['onPeerNotRequired'];
  private readonly onPeerDisconnected?: RawServiceMeshRuntimeOptions['onPeerDisconnected'];
  private readonly onProtocolError?: RawServiceMeshRuntimeOptions['onProtocolError'];
  private descriptor: ServiceNodeDescriptor;
  private host?: ZLinkRawHostPort;
  private router?: ZLinkRawRouterPort;
  private monitor?: ZLinkRawMonitorPort;
  private nextCorrelation = 1n;
  private serviceIngress?: RawServiceIngressHandler;
  private closed = false;

  constructor(options: RawServiceMeshRuntimeOptions) {
    this.descriptor = options.descriptor;
    this.topology = new ServiceTopologyRegistry(options.descriptor);
    this.mailbox = new ServiceMailbox(undefined, options.onMailboxReady);
    this.liveness = new ServiceLivenessRegistry(options.probeIntervalMs, options.peerTimeoutMs);
    this.bindingPort = options.bindingPort;
    if (options.applicationJobQueue === undefined) {
      throw new TypeError('Raw service runtime requires the host Application Job Queue.');
    }
    this.applicationJobQueue = options.applicationJobQueue;
    this.onPeerNotRequired = options.onPeerNotRequired;
    this.onPeerDisconnected = options.onPeerDisconnected;
    this.onProtocolError = options.onProtocolError;
  }

  start(): void {
    if (this.router !== undefined) return;
    if (this.closed) throw new Error('Raw service runtime cannot restart after close.');
    const host = this.bindingPort.createHost();
    try {
      const router = host.createRouter();
      router.setRoutingId(this.descriptor.nodeRoutingId);
      router.bind(this.descriptor.advertisedEndpoint);
      const next = {
        ...this.descriptor,
        advertisedEndpoint: router.localEndpoint(),
        descriptorRevision: this.descriptor.descriptorRevision + 1n,
        state: 'serving' as const
      };
      this.topology.publishLocal(next);
      this.descriptor = next;
      this.monitor = router.monitor(event => {
        this.observeMonitorEvent(event);
        if (this.monitorEvents.length < MAX_PENDING_MONITOR_EVENTS) {
          this.monitorEvents.push(event);
        } else {
          // Keep one latest transition per physical connection when the
          // normal drain is delayed. This bounds storage without replacing an
          // unrelated connection's transition.
          const existingIndex = this.monitorEvents.findIndex(candidate =>
            sameMonitorConnection(candidate, event)
          );
          if (existingIndex >= 0) {
            this.monitorEvents[existingIndex] = event;
          } else {
            // Preserve the newest lifecycle edge when unrelated connections
            // saturate the bounded queue; stale edges are less useful than
            // the current edge and will be reconciled by the next monitor
            // drain.
            this.monitorEvents.shift();
            this.monitorEvents.push(event);
          }
        }
      });
      this.host = host;
      this.router = router;
    } catch (error) {
      host.close();
      throw error;
    }
  }

  connectPeer(endpoint: string, expected: ServiceNodeDescriptor): void {
    this.requireStarted().connectToRoutingId(expected.nodeRoutingId, endpoint);
    this.expectedPeers.set(expected.nodeRoutingId, {
      meshName: expected.meshName,
      nodeRoutingId: expected.nodeRoutingId,
      endpoint,
      securityIdentity: expected.securityIdentity,
      lifecycleGeneration: expected.lifecycleGeneration
    });
  }

  connectPeerByRoutingId(
    endpoint: string,
    nodeRoutingId: string,
    securityIdentity?: string,
    lifecycleGeneration?: bigint
  ): void {
    this.requireStarted().connectToRoutingId(nodeRoutingId, endpoint);
    this.expectedPeers.set(nodeRoutingId, {
      meshName: this.topology.localDescriptor().meshName,
      nodeRoutingId,
      endpoint,
      securityIdentity,
      lifecycleGeneration
    });
  }

  expectPeerByRoutingId(
    endpoint: string,
    nodeRoutingId: string,
    securityIdentity?: string,
    lifecycleGeneration?: bigint
  ): void {
    this.expectedPeers.set(nodeRoutingId, {
      meshName: this.topology.localDescriptor().meshName,
      nodeRoutingId,
      endpoint,
      securityIdentity,
      lifecycleGeneration
    });
  }

  connectPeerEndpoint(endpoint: string): void {
    this.requireStarted().connect(endpoint);
    this.endpointOnlyPeers.add(endpoint);
  }

  disconnectPeer(
    endpoint: string,
    nodeRoutingId: string,
    lifecycleGeneration?: bigint
  ): void {
    const current = this.topology.peer(nodeRoutingId);
    const expected = this.expectedPeers.get(nodeRoutingId);
    if (
      lifecycleGeneration !== undefined
      && (
        (
          current !== undefined
          && current.descriptor.lifecycleGeneration !== lifecycleGeneration
        )
        || (
          current === undefined
          && expected?.lifecycleGeneration !== lifecycleGeneration
        )
      )
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
        lifecycleGeneration === undefined
        || expected?.lifecycleGeneration === undefined
        || expected.lifecycleGeneration === lifecycleGeneration
      ) {
        this.expectedPeers.delete(nodeRoutingId);
      }
    }
  }

  disconnectPeerEndpoint(endpoint: string): void {
    try {
      this.requireStarted().disconnect(endpoint);
    } catch (error) {
      if (!isAlreadyDisconnectedError(error)) throw error;
    } finally {
      this.endpointOnlyPeers.delete(endpoint);
      for (const peer of this.topology.peers()) {
        if (peer.descriptor.advertisedEndpoint === endpoint) {
          this.removePeer(peer);
        }
      }
    }
  }

  async announcePeer(nodeRoutingId: string): Promise<boolean> {
    if (!this.expectedPeers.has(nodeRoutingId)) return false;
    return this.send(
      nodeRoutingId,
      [encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())]
    );
  }

  isPeerRouteReady(nodeRoutingId: string, lifecycleGeneration?: bigint): boolean {
    const peer = this.topology.peer(nodeRoutingId);
    const monitorConnection = this.monitorConnectionStates.get(nodeRoutingId);
    const applicationRouteReady = monitorConnection === undefined
      || monitorConnection === peer?.connectionId;
    const ready = peer !== undefined
      && (lifecycleGeneration === undefined
        || peer.descriptor.lifecycleGeneration === lifecycleGeneration)
      && applicationRouteReady
      && this.liveness.isReady(nodeRoutingId, peer.connectionId);
    return ready;
  }

  async announceExpectedPeers(): Promise<number> {
    let accepted = 0;
    for (const nodeRoutingId of this.expectedPeers.keys()) {
      // Admission is a one-time fence for the current physical connection.
      // Re-sending Hello after the peer is admitted would re-run admission on
      // every poll and reset the liveness record before application traffic
      // can use the route. A disconnected peer is removed by monitor handling
      // and remains eligible for the next admission attempt.
      if (this.topology.peer(nodeRoutingId) !== undefined) continue;
      if (await this.announcePeer(nodeRoutingId)) accepted++;
    }
    return accepted;
  }

  async updateLocalWeights(options: {
    readonly placementWeight?: number;
    readonly channelName?: string;
    readonly channelWeight?: number;
  }): Promise<void> {
    const current = this.topology.localDescriptor();
    const channels = options.channelName === undefined
      ? current.channels
      : current.channels.map(channel =>
          channel.name === options.channelName
            ? { ...channel, weight: options.channelWeight! }
            : channel);
    const next = {
      ...current,
      descriptorRevision: current.descriptorRevision + 1n,
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
    await this.announceExpectedPeers();
  }

  replaceDiscoveredNotRequired(
    descriptors: readonly ServiceNodeDescriptor[]
  ): void {
    this.topology.replaceDiscoveredNotRequired(descriptors);
  }

  isObjectClientNodeDirectTarget(nodeRoutingId: string): boolean {
    const descriptor = nodeRoutingId === this.descriptor.nodeRoutingId
      ? this.topology.localDescriptor()
      : this.topology.knownDescriptor(nodeRoutingId);
    return descriptor?.objectRole === 'client';
  }

  async sendToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayload
  ): Promise<boolean> {
    return this.send(
      targetNodeRoutingId,
      [encodeNodeSendHeader(), encodeApplicationPayload(payload)]
    );
  }

  async sendToChannel(
    channelName: string,
    payload: ServiceApplicationPayload
  ): Promise<boolean> {
    const selected = this.topology.selectChannel(
      channelName,
      peer => this.isLocalOrReadyPeer(peer.descriptor.nodeRoutingId)
    );
    if (selected === undefined) return false;
    if (selected.descriptor.nodeRoutingId === this.descriptor.nodeRoutingId) {
      const applicationJobOwner = await this.reserveLocalIngress();
      try {
        const applicationJob = await applicationJobOwner.acquire('application');
        const accepted = this.mailbox.tryEnqueue({
          owner: `channel:${channelName}`,
          domain: 'application',
          parts: [encodeChannelSendHeader(channelName), encodeApplicationPayload(payload)],
          sourceRoutingId: this.descriptor.nodeRoutingId,
          applicationJob
        });
        if (!accepted) applicationJob.close();
        return accepted;
      } finally {
        applicationJobOwner.close();
      }
    }
    return this.send(selected.descriptor.nodeRoutingId, [
      encodeChannelSendHeader(channelName),
      encodeApplicationPayload(payload)
    ]);
  }

  requestToNode(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayload,
    timeoutMs: number
  ): PendingOperation<RawServiceRequestResult> {
    return this.requestToTarget(targetNodeRoutingId, payload, timeoutMs);
  }

  requestToChannel(
    channelName: string,
    payload: ServiceApplicationPayload,
    timeoutMs: number
  ): PendingOperation<RawServiceRequestResult> | undefined {
    const selected = this.topology.selectChannel(
      channelName,
      peer => this.isLocalOrReadyPeer(peer.descriptor.nodeRoutingId)
    );
    return selected === undefined
      ? undefined
      : this.requestToTarget(selected.descriptor.nodeRoutingId, payload, timeoutMs, channelName);
  }

  private isLocalOrReadyPeer(nodeRoutingId: string): boolean {
    return nodeRoutingId === this.descriptor.nodeRoutingId
      || this.isPeerRouteReady(nodeRoutingId);
  }

  setServiceIngress(handler: RawServiceIngressHandler): void {
    if (this.serviceIngress !== undefined && this.serviceIngress !== handler) {
      throw new Error('Raw service ingress is already registered.');
    }
    this.serviceIngress = handler;
  }

  async reserveLocalIngress(
    signal?: AbortSignal
  ): Promise<ApplicationIngressRecordOwner> {
    const waitSignal = signal === undefined
      ? this.applicationJobStop.signal
      : AbortSignal.any([this.applicationJobStop.signal, signal]);
    const permit = await this.applicationJobQueue.acquire(waitSignal);
    return ApplicationIngressRecordOwner.create(
      this.applicationJobQueue,
      permit,
      { close() {} }
    );
  }

  async sendService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[]
  ): Promise<boolean> {
    return this.send(targetNodeRoutingId, parts);
  }

  requestService(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]> {
    return this.requireStarted().request(targetNodeRoutingId, parts, timeoutMs);
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
    if (record.reply !== undefined) {
      record.reply(parts);
    } else {
      this.requireStarted().reply(
        record.sourceRoute ?? record.sourceRoutingId,
        record.requestSequence,
        parts
      );
    }
  }

  reply(
    request: ServiceMailboxRecord,
    payload: ServiceApplicationPayload,
    terminalResult = 0,
    failureCode = 0
  ): void {
    if (request.localReply !== undefined) {
      request.localReply(
        terminalResult,
        failureCode,
        terminalResult === 0 ? payload : undefined
      );
      return;
    }
    if (request.reply !== undefined) {
      request.reply([
        encodeReplyHeader(request.correlation!, terminalResult, failureCode),
        ...(terminalResult === 0 ? [encodeApplicationPayload(payload)] : [])
      ]);
      return;
    }
    if (
      request.sourceRoutingId === undefined
      || request.requestSequence === undefined
      || request.correlation === undefined
    ) {
      throw new TypeError('Reply requires a request mailbox record.');
    }
    this.requireStarted().reply(
      request.sourceRoute ?? request.sourceRoutingId,
      request.requestSequence,
      [
        encodeReplyHeader(request.correlation, terminalResult, failureCode),
        ...(terminalResult === 0 ? [encodeApplicationPayload(payload)] : [])
      ]
    );
  }

  async pumpOne(
    nowMs = performance.now(),
    observe?: RawServicePumpObserver
  ): Promise<RawServicePumpResult> {
    const router = this.requireStarted();
    let permit: ApplicationJobPermitPort;
    try {
      permit = await this.applicationJobQueue.acquire(this.applicationJobStop.signal);
    } catch (error) {
      if (this.closed || this.applicationJobStop.signal.aborted) return 'noData';
      throw error;
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
      const correlation = header.command === M6aServiceWireCommand.nodeRequest
        ? decodeNodeRequestHeader(received.parts[0]!)
        : header.command === M6aServiceWireCommand.channelRequest
          ? decodeChannelRequestHeader(received.parts[0]!).correlation
          : undefined;
      if (correlation === undefined) return false;
      const reply = [encodeReplyHeader(
        correlation,
        RequestResult.ProtocolError,
        ServiceWireFrameworkErrorCode.requestProtocolError
      )];
      if (received.reply !== undefined) {
        received.reply(reply);
      } else {
        this.requireStarted().reply(
          received.sourceRoute,
          received.requestSeq,
          reply
        );
      }
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
      received.parts.length === 0
      || (
        received.parts.length === 1
        && received.parts[0]!.byteLength === 0
      )
    ) {
      return await this.send(
        received.sourceRid,
        [encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())]
      )
        ? 'infrastructure'
        : 'dropped';
    }
    try {
      const header = decodeHeader(received.parts[0]!);
      if (
        header.command === M6aServiceWireCommand.hello
        || header.command === M6aServiceWireCommand.admit
        || header.command === M6aServiceWireCommand.update
      ) {
        if (received.parts.length !== 1) return 'protocolError';
        const descriptor = decodeRouteMeshAdmission(
          received.parts[0]!,
          header.command,
          received.sourceRid
        );
        const expected = this.expectedPeers.get(received.sourceRid);
        if (
          expected !== undefined
          && (
            expected.meshName !== descriptor.meshName
            || expected.nodeRoutingId !== descriptor.nodeRoutingId
          )
        ) {
          await this.send(received.sourceRid, [encodeReject(3)]);
          return 'infrastructure';
        }
        const connection = this.currentConnectionCandidate(
          received.sourceRid,
          descriptor.advertisedEndpoint,
          received.transportPairId,
          received.transportPairGeneration
        );
        const result = this.admitPeer(
          descriptor,
          connection,
          nowMs,
          expected
        );
        if (result !== 'admitted') {
          await this.send(received.sourceRid, [encodeReject(admissionReason(result))]);
          if (result === 'notRequired') {
            this.retireNotRequiredExpectedPeer(
              received.sourceRid,
              descriptor.advertisedEndpoint
            );
          }
          return 'infrastructure';
        }
        this.selectBilateralConnection(descriptor);
        if (header.command === M6aServiceWireCommand.hello) {
          await this.send(
            received.sourceRid,
            [encodeRouteMeshAdmission(M6aServiceWireCommand.admit, this.topology.localDescriptor())]
          );
        }
        return 'infrastructure';
      }
      if (header.command === M6aServiceWireCommand.reject) {
        if (received.parts.length !== 1) return 'protocolError';
        const reason = decodeReject(received.parts[0]!);
        // A reject belongs to one physical candidate, but the wire record does
        // not carry that candidate's connection id. Keep an already admitted
        // peer: bilateral manual connect can reject the duplicate after the
        // other candidate has become Ready.
        if (reason === 4 && this.topology.peer(received.sourceRid) === undefined) {
          const expected = this.expectedPeers.get(received.sourceRid);
          const local = this.topology.localDescriptor();
          this.topology.markNotRequired({
            ...local,
            nodeRoutingId: received.sourceRid,
            lifecycleGeneration: 1n,
            descriptorRevision: 1n,
            advertisedEndpoint: expected?.endpoint ?? local.advertisedEndpoint,
            channels: [],
            objectRole: 'client'
          });
          this.retireNotRequiredExpectedPeer(received.sourceRid);
        }
        return 'infrastructure';
      }
      const peer = this.topology.peer(received.sourceRid);
      if (peer === undefined) return 'protocolError';
      if (
        header.command === M6aServiceWireCommand.livenessProbe
        || header.command === M6aServiceWireCommand.livenessAck
      ) {
        if (received.parts.length !== 1) return 'protocolError';
        const record = livenessCodec.decodeLivenessRecord(received.parts[0]!);
        if (record.command === M6aServiceWireCommand.livenessProbe) {
          const ack = this.liveness.acknowledgeProbe(
            received.sourceRid,
            peer.connectionId,
            record.probeId
          );
          const sent = ack !== undefined && await this.send(
            received.sourceRid,
            [livenessCodec.encodeLivenessRecord({
              command: M6aServiceWireCommand.livenessAck,
              probeId: record.probeId
            })]
          );
          if (!sent) {
            return 'protocolError';
          }
        } else {
          this.liveness.acknowledge(
            received.sourceRid,
            peer.connectionId,
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
        header.flags !== 0
        || received.parts.length !== 2
        || ![
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
        this.replyService({
          sourceRoutingId: received.sourceRid,
          sourceRoute: received.sourceRoute,
          requestSequence: received.requestSeq,
          ...(received.reply === undefined ? {} : { reply: received.reply })
        }, [
          encodeReplyHeader(
            correlation,
            RequestResult.NotConnected,
            0
          )
        ]);
        return 'infrastructure';
      }
      return 'dropped';
    } catch (error) {
      if (error instanceof ServiceWireProtocolError) return 'protocolError';
      throw error;
    }
  }

  async tickLiveness(nowMs = performance.now()): Promise<ServiceLivenessTick> {
    const result = this.liveness.tick(nowMs);
    this.requireStarted();
    for (const probe of result.probes) {
      await this.send(probe.nodeRoutingId, [livenessCodec.encodeLivenessRecord({
        command: M6aServiceWireCommand.livenessProbe,
        probeId: probe.probeId
      })]);
    }
    for (const nodeRoutingId of result.timedOutNodes) {
      const peer = this.topology.peer(nodeRoutingId);
      if (peer === undefined) continue;
      // Liveness timeout is a semantic peer removal.  Route topology,
      // physical candidate state, and auto-connect intent must be retired by
      // the same owner so a failed endpoint cannot be selected again.
      this.removeConnectionCandidate(nodeRoutingId, peer.connectionId);
      this.removePeer(peer);
    }
    return result;
  }

  async drainMonitorEvents(nowMs = performance.now()): Promise<number> {
    let handled = 0;
    // Detach the current batch so monitor callbacks that run while an event is
    // being handled append to the next batch without copying or reindexing it.
    const events = this.monitorEvents;
    this.monitorEvents = this.monitorDrainBuffer;
    this.monitorDrainBuffer = events;
    this.monitorEvents.length = 0;
    for (const event of events) {
      handled++;
      const nodeRoutingId = event.routingId
        ?? this.expectedPeerRoutingId(event.remoteAddress);
      if (event.event === MONITOR_CONNECTION_READY && isConnectionReadyEdge(event)) {
        if (nodeRoutingId === undefined) {
          const candidate = this.createUnresolvedConnectionCandidate(event);
          const alreadyQueued = this.unresolvedConnectionCandidates.some(
            value => value.connectionId === candidate.connectionId
          );
          if (!alreadyQueued && this.unresolvedConnectionCandidates.length < MAX_UNRESOLVED_CONNECTION_CANDIDATES) {
            this.unresolvedConnectionCandidates.push(candidate);
          }
          continue;
        }
        if (!this.acceptsExpectedMonitorEndpoint(nodeRoutingId, event)) {
          this.disconnectUnexpectedMonitorPair(event);
          continue;
        }
        const candidate = this.createConnectionCandidate(nodeRoutingId, event);
        const existingPeer = this.topology.peer(nodeRoutingId);
        if (existingPeer !== undefined && existingPeer.connectionId === candidate.connectionId) {
          this.monitorConnectionStates.set(nodeRoutingId, candidate.connectionId);
          continue;
        }
        if (existingPeer !== undefined) {
          if (this.liveness.isReady(nodeRoutingId, existingPeer.connectionId)) {
            const existingCandidate = this.connectionCandidates
              .get(nodeRoutingId)?.get(existingPeer.connectionId);
            if (existingPeer.connectionId.startsWith('unmonitored:')
              && candidate.transportPairId !== undefined
              && candidate.transportPairGeneration !== undefined) {
              // The fallback endpoint admission can precede the physical
              // pair READY edge. Retain the pair as provisional evidence and
              // announce over that pair so the peer descriptor admission can
              // promote the semantic route before an application request.
              let provisionalCandidates = this.connectionCandidates.get(nodeRoutingId);
              if (provisionalCandidates === undefined) {
                provisionalCandidates = new Map();
                this.connectionCandidates.set(nodeRoutingId, provisionalCandidates);
              }
              provisionalCandidates.set(candidate.connectionId, candidate);
              await this.announcePeer(nodeRoutingId);
              continue;
            }
            const sameTransportPair = existingCandidate !== undefined
              && candidate.transportPairId !== undefined
              && candidate.transportPairGeneration !== undefined
              && existingCandidate.transportPairId === candidate.transportPairId
              && existingCandidate.transportPairGeneration === candidate.transportPairGeneration;
            if (sameTransportPair) {
              this.removeConnectionCandidate(nodeRoutingId, candidate.connectionId);
              this.monitorConnectionStates.set(nodeRoutingId, existingPeer.connectionId);
              continue;
            }
            // A ready route is the current semantic owner. A second physical
            // candidate may be admitted only after the current lease expires;
            // this also prevents a late old-process reconnect from replacing
            // the ready replacement by arrival order.
            this.disconnectUnexpectedMonitorPair(candidate);
            continue;
          }
          // Keep the physical candidate until the wire descriptor decides
          // whether it is the current logical peer. READY events can race
          // with DISCONNECTED events during reconnect; rejecting the READY
          // here can terminate the replacement before admission evaluates its
          // lifecycle generation.
          let provisionalCandidates = this.connectionCandidates.get(nodeRoutingId);
          if (provisionalCandidates === undefined) {
            provisionalCandidates = new Map();
            this.connectionCandidates.set(nodeRoutingId, provisionalCandidates);
          }
          provisionalCandidates.set(candidate.connectionId, candidate);
          this.connectionIds.set(nodeRoutingId, candidate.connectionId);
          await this.announcePeer(nodeRoutingId);
          continue;
        }
        let candidates = this.connectionCandidates.get(nodeRoutingId);
        if (candidates === undefined) {
          candidates = new Map();
          this.connectionCandidates.set(nodeRoutingId, candidates);
        }
        candidates.set(candidate.connectionId, candidate);
        // Before wire admission the monitor candidate is the only physical
        // route evidence available to the admission message.
        this.connectionIds.set(nodeRoutingId, candidate.connectionId);
        await this.announcePeer(nodeRoutingId);
      } else if (
        event.event === MONITOR_DISCONNECTED
        && nodeRoutingId !== undefined
      ) {
        const peer = this.topology.peer(nodeRoutingId);
        const disconnectedId = monitorConnectionId(event, nodeRoutingId);
        this.removeConnectionCandidate(nodeRoutingId, disconnectedId);
        if (peer !== undefined && peer.connectionId === disconnectedId) {
          this.removePeer(peer);
        }
      } else if (
        event.event === MONITOR_DISCONNECTED
      ) {
        const disconnectedId = monitorConnectionId(event);
        const index = this.unresolvedConnectionCandidates.findIndex(
          candidate => candidate.connectionId === disconnectedId
        );
        if (index >= 0) this.unresolvedConnectionCandidates.splice(index, 1);
      }
    }
    this.monitorDrainBuffer.length = 0;
    void nowMs;
    return handled;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.applicationJobStop.abort(
      new Error('Raw service runtime application job admission stopped.')
    );
    this.mailbox.close();
    this.operations.close('Raw service runtime closed.');
    this.serviceIngress = undefined;
    this.monitor?.close();
    this.monitor = undefined;
    const host = this.host;
    this.router = undefined;
    this.host = undefined;
    if (host !== undefined) host.close();
  }

  private requestToTarget(
    targetNodeRoutingId: string,
    payload: ServiceApplicationPayload,
    timeoutMs: number,
    channelName?: string
  ): PendingOperation<RawServiceRequestResult> {
    const correlation = this.nextCorrelation++;
    const header = channelName === undefined
      ? encodeNodeRequestHeader(correlation)
      : encodeChannelRequestHeader(correlation, channelName);
    const encodedPayload = encodeApplicationPayload(payload);
    const pending = this.operations.reserve(timeoutMs);
    let selectedTargetNodeRoutingId = targetNodeRoutingId;
    if (
      selectedTargetNodeRoutingId !== this.descriptor.nodeRoutingId
      && !this.isPeerRouteReady(selectedTargetNodeRoutingId)
    ) {
      // Channel selection happens before native admission. If that selection
      // becomes stale during a peer drain, choose another ready channel peer
      // while no request has been submitted yet. Reusing the same pending
      // operation and correlation keeps this recovery pre-admission and
      // cannot duplicate an application request.
      if (channelName !== undefined) {
        const excludedTargets = new Set([selectedTargetNodeRoutingId]);
        for (;;) {
          const alternate = this.topology.selectChannel(
            channelName,
            peer => {
              const candidate = peer.descriptor.nodeRoutingId;
              return !excludedTargets.has(candidate)
                && this.isLocalOrReadyPeer(candidate);
            }
          );
          if (alternate === undefined) break;
          selectedTargetNodeRoutingId = alternate.descriptor.nodeRoutingId;
          if (
            selectedTargetNodeRoutingId === this.descriptor.nodeRoutingId
            || this.isPeerRouteReady(selectedTargetNodeRoutingId)
          ) {
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
            owner: channelName === undefined
              ? `node:${this.descriptor.nodeRoutingId}`
              : `channel:${channelName}`,
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
      })().catch(error => this.operations.fail(pending.id, error));
      return pending;
    }
    if (!this.isPeerRouteReady(selectedTargetNodeRoutingId)) {
      // Preserve Core's distinct target-not-found result for an RID the
      // topology has never observed. A known peer without a usable route is
      // a Framework NotConnected result; an unknown RID is completed locally
      // as RequestTargetNotFound without submitting an application frame.
      const knownTarget = this.topology.peer(selectedTargetNodeRoutingId) !== undefined
        || this.topology.knownDescriptor(selectedTargetNodeRoutingId) !== undefined;
      this.operations.complete(pending.id, {
        terminalResult: knownTarget
          ? RequestResult.NotConnected
          : RequestResult.NotFound,
        failureCode: knownTarget ? 0 : REQUEST_TARGET_NOT_FOUND_FAILURE_CODE
      });
      return pending;
    }
    const parts = [header, encodedPayload];
    let router: ZLinkRawRouterPort;
    let request: Promise<readonly Uint8Array[]>;
    try {
      router = this.requireStarted();
      request = router.request(
        selectedTargetNodeRoutingId,
        parts,
        timeoutMs
      );
    } catch (error) {
      this.operations.fail(pending.id, error);
      return pending;
    }
    void request.then(
      replyParts => {
        try {
          if (replyParts.length < 1 || replyParts.length > 2) {
            throw new ServiceWireProtocolError('Invalid reply parts.');
          }
          const reply = decodeReplyHeader(replyParts[0]!);
          if (reply.correlation !== correlation) throw new ServiceWireProtocolError('Reply correlation mismatch.');
          if (reply.tail.byteLength !== 0) {
            throw new ServiceWireProtocolError('Generic node/channel reply carries an operation-specific tail.');
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
              ? { ...result, payload: decodeApplicationPayload(replyParts[1]!) }
              : result
          );
        } catch (error) {
          this.operations.fail(pending.id, error);
        }
      },
      error => {
        this.operations.fail(pending.id, error);
      }
    );
    return pending;
  }

  private admitPeer(
    descriptor: ServiceNodeDescriptor,
    connection: PhysicalConnectionCandidate,
    nowMs: number,
    expected?: {
      readonly endpoint?: string;
      readonly securityIdentity?: string;
      readonly lifecycleGeneration?: bigint;
    }
  ): PeerAdmissionResult {
    const previous = this.topology.peer(descriptor.nodeRoutingId);
    if (
      previous !== undefined
      && previous.descriptor.lifecycleGeneration === descriptor.lifecycleGeneration
      && (
        descriptor.descriptorRevision < previous.descriptor.descriptorRevision
        || (
          descriptor.descriptorRevision === previous.descriptor.descriptorRevision
          && !sameServiceNodeDescriptor(previous.descriptor, descriptor)
        )
      )
    ) {
      throw new ServiceWireProtocolError(
        `RouteMesh peer '${descriptor.nodeRoutingId}' descriptor revision is stale or conflicting.`
      );
    }
    const result = this.topology.admit(
      descriptor,
      connection.connectionId,
      expected,
      connection.discriminator
    );
    if (result === 'admitted') {
      this.connectionIds.set(
        descriptor.nodeRoutingId,
        connection.connectionId
      );
      this.liveness.admit(descriptor.nodeRoutingId, connection.connectionId, nowMs);
      this.liveness.requestProbe(
        descriptor.nodeRoutingId,
        connection.connectionId,
        nowMs
      );
      if (connection.transportPairId !== undefined
        && connection.transportPairGeneration !== undefined) {
        this.monitorConnectionStates.set(
          descriptor.nodeRoutingId,
          connection.connectionId
        );
      }
      if (previous !== undefined && previous.connectionId !== connection.connectionId) {
        const previousCandidate = this.connectionCandidates
          .get(descriptor.nodeRoutingId)?.get(previous.connectionId);
        if (previousCandidate !== undefined) {
          const sameTransportPair = connection.transportPairId !== undefined
            && connection.transportPairGeneration !== undefined
            && previousCandidate.transportPairId === connection.transportPairId
            && previousCandidate.transportPairGeneration === connection.transportPairGeneration;
          if (!sameTransportPair) this.disconnectUnexpectedMonitorPair(previousCandidate);
          this.removeConnectionCandidate(
            descriptor.nodeRoutingId,
            previousCandidate.connectionId
          );
        }
      }
    } else if (result === 'notRequired' && previous !== undefined) {
      this.liveness.disconnect(
        descriptor.nodeRoutingId,
        previous.connectionId
      );
    } else if (previous !== undefined) {
      this.connectionIds.set(
        descriptor.nodeRoutingId,
        previous.connectionId
      );
      if (connection.connectionId !== previous.connectionId) {
        this.disconnectUnexpectedMonitorPair(connection);
        this.removeConnectionCandidate(
          descriptor.nodeRoutingId,
          connection.connectionId
        );
      }
    }
    return result;
  }

  private currentConnectionCandidate(
    nodeRoutingId: string,
    advertisedEndpoint: string,
    transportPairId?: bigint,
    transportPairGeneration?: bigint
  ): PhysicalConnectionCandidate {
    if (transportPairId !== undefined
      && transportPairGeneration !== undefined
      && transportPairId !== 0n
      && transportPairGeneration !== 0n) {
      const candidates = this.connectionCandidates.get(nodeRoutingId);
      const exactId = JSON.stringify([
        nodeRoutingId,
        'transport-pair',
        transportPairId.toString(),
        transportPairGeneration.toString()
      ]);
      const exact = candidates?.get(exactId);
      if (exact !== undefined) return exact;
      const localRid = this.topology.localDescriptor().nodeRoutingId;
      const endpointOnly = this.endpointOnlyPeers.has(advertisedEndpoint);
      const direction: PhysicalConnectionDirection = this.expectedPeers.has(nodeRoutingId)
        ? 'outbound'
        : endpointOnly
          ? localRid.localeCompare(nodeRoutingId) <= 0 ? 'outbound' : 'inbound'
          : 'inbound';
      const initiator = endpointOnly && localRid.localeCompare(nodeRoutingId) > 0
        ? nodeRoutingId
        : direction === 'outbound' ? localRid : nodeRoutingId;
      const candidate: PhysicalConnectionCandidate = {
        connectionId: exactId,
        direction,
        discriminator: `initiator:${initiator}`,
        localAddress: '',
        remoteAddress: advertisedEndpoint,
        transportPairId,
        transportPairGeneration
      };
      const exactCandidates = candidates ?? new Map<string, PhysicalConnectionCandidate>();
      if (candidates === undefined) this.connectionCandidates.set(nodeRoutingId, exactCandidates);
      exactCandidates.set(exactId, candidate);
      return candidate;
    }
    let connectionId = this.connectionIds.get(nodeRoutingId);
    let candidate = connectionId === undefined
      ? undefined
      : this.connectionCandidates.get(nodeRoutingId)?.get(connectionId);
    if (candidate === undefined) {
      candidate = this.promoteUnresolvedConnectionCandidate(
        nodeRoutingId,
        advertisedEndpoint
      );
      connectionId = candidate?.connectionId;
    }
    if (candidate === undefined) {
      const localRid = this.topology.localDescriptor().nodeRoutingId;
      const endpointOnly = this.endpointOnlyPeers.has(advertisedEndpoint);
      const direction: PhysicalConnectionDirection =
        this.expectedPeers.has(nodeRoutingId)
          ? 'outbound'
          : endpointOnly
            ? localRid.localeCompare(nodeRoutingId) <= 0
              ? 'outbound'
              : 'inbound'
            : 'inbound';
      const initiator = endpointOnly && localRid.localeCompare(nodeRoutingId) > 0
        ? nodeRoutingId
        : direction === 'outbound'
          ? localRid
          : nodeRoutingId;
      connectionId = `unmonitored:${direction}:${randomUUID()}`;
      candidate = {
        connectionId,
        direction,
        discriminator: `initiator:${initiator}`,
        localAddress: '',
        remoteAddress: advertisedEndpoint
      };
      let candidates = this.connectionCandidates.get(nodeRoutingId);
      if (candidates === undefined) {
        candidates = new Map();
        this.connectionCandidates.set(nodeRoutingId, candidates);
      }
      candidates.set(connectionId, candidate);
      this.connectionIds.set(nodeRoutingId, connectionId);
    }
    return candidate;
  }

  private createConnectionCandidate(
    nodeRoutingId: string,
    event: ZLinkRawMonitorRecord
  ): PhysicalConnectionCandidate {
    const local = this.topology.localDescriptor();
    const expected = this.expectedPeers.get(nodeRoutingId);
    const direction: PhysicalConnectionDirection =
      event.localAddress === local.advertisedEndpoint
        ? 'inbound'
        : expected?.endpoint === event.remoteAddress
          ? 'outbound'
          : this.endpointOnlyPeers.has(event.remoteAddress)
            ? 'outbound'
            : 'unknown';
    const initiator = direction === 'outbound'
      ? local.nodeRoutingId
      : direction === 'inbound'
        ? nodeRoutingId
        : undefined;
    return {
      connectionId: monitorConnectionId(event, nodeRoutingId),
      direction,
      discriminator: initiator === undefined
        ? `unknown:${monitorConnectionId(event, nodeRoutingId)}`
        : `initiator:${initiator}`,
      localAddress: event.localAddress,
      remoteAddress: event.remoteAddress,
      transportPairId: event.transportPairId,
      transportPairGeneration: event.transportPairGeneration
    };
  }

  private createUnresolvedConnectionCandidate(
    event: ZLinkRawMonitorRecord
  ): PhysicalConnectionCandidate {
    const connectionId = monitorConnectionId(event);
    const local = this.topology.localDescriptor();
    const direction: PhysicalConnectionDirection =
      this.endpointOnlyPeers.has(event.remoteAddress)
          || this.expectedPeerRoutingId(event.remoteAddress) !== undefined
        ? 'outbound'
        : event.localAddress === local.advertisedEndpoint
          ? 'inbound'
          : 'unknown';
    return {
      connectionId,
      direction,
      discriminator: `unresolved:${connectionId}`,
      localAddress: event.localAddress,
      remoteAddress: event.remoteAddress,
      transportPairId: event.transportPairId,
      transportPairGeneration: event.transportPairGeneration
    };
  }

  private promoteUnresolvedConnectionCandidate(
    nodeRoutingId: string,
    advertisedEndpoint: string
  ): PhysicalConnectionCandidate | undefined {
    const relevant = this.unresolvedConnectionCandidates
      .map((candidate, index) => ({ candidate, index }))
      .filter(({ candidate }) =>
        candidate.remoteAddress === advertisedEndpoint
        || candidate.direction === 'inbound');
    if (relevant.length === 0) return undefined;
    const directions = new Set(relevant.map(value => value.candidate.direction));
    const localRid = this.topology.localDescriptor().nodeRoutingId;
    const preferredDirection: PhysicalConnectionDirection =
      directions.has('inbound') && directions.has('outbound')
        ? localRid.localeCompare(nodeRoutingId) <= 0
          ? 'outbound'
          : 'inbound'
        : relevant.at(-1)!.candidate.direction;
    const selected = [...relevant]
      .reverse()
      .find(value => value.candidate.direction === preferredDirection)
      ?? relevant.at(-1)!;
    this.unresolvedConnectionCandidates.splice(selected.index, 1);
    const initiator = selected.candidate.direction === 'outbound'
      ? localRid
      : selected.candidate.direction === 'inbound'
        ? nodeRoutingId
        : undefined;
    const candidate: PhysicalConnectionCandidate = {
      ...selected.candidate,
      discriminator: initiator === undefined
        ? `unknown:${selected.candidate.connectionId}`
        : `initiator:${initiator}`
    };
    let candidates = this.connectionCandidates.get(nodeRoutingId);
    if (candidates === undefined) {
      candidates = new Map();
      this.connectionCandidates.set(nodeRoutingId, candidates);
    }
    candidates.set(candidate.connectionId, candidate);
    this.connectionIds.set(nodeRoutingId, candidate.connectionId);
    return candidate;
  }

  private removeConnectionCandidate(nodeRoutingId: string, connectionId: string): void {
    const candidates = this.connectionCandidates.get(nodeRoutingId);
    candidates?.delete(connectionId);
    if (candidates?.size === 0) this.connectionCandidates.delete(nodeRoutingId);
    if (this.connectionIds.get(nodeRoutingId) === connectionId) {
      this.connectionIds.delete(nodeRoutingId);
    }
  }

  private expectedPeerRoutingId(endpoint: string): string | undefined {
    for (const [nodeRoutingId, expected] of this.expectedPeers) {
      if (expected.endpoint === endpoint) return nodeRoutingId;
    }
    return undefined;
  }

  private acceptsExpectedMonitorEndpoint(
    nodeRoutingId: string,
    event: Pick<ZLinkRawMonitorRecord, 'remoteAddress' | 'routingId'>
  ): boolean {
    // A monitor address is the physical socket address, which may be an
    // ephemeral port. Once Core has resolved the peer routing ID, that ID is
    // the semantic identity; comparing the physical address with the
    // discovery advertised endpoint would reject valid READY events.
    if (event.routingId === nodeRoutingId) return true;
    const expected = this.expectedPeers.get(nodeRoutingId);
    return expected === undefined || expected.endpoint === undefined
      || expected.endpoint === event.remoteAddress;
  }

  private disconnectUnexpectedMonitorPair(
    event: Pick<ZLinkRawMonitorRecord, 'transportPairId' | 'transportPairGeneration'>
  ): void {
    const router = this.router;
    if (
      router?.disconnectTransportPair === undefined
      || typeof event.transportPairId !== 'bigint'
      || typeof event.transportPairGeneration !== 'bigint'
      || event.transportPairId === 0n
      || event.transportPairGeneration === 0n
    ) return;
    try {
      router.disconnectTransportPair(
        event.transportPairId,
        event.transportPairGeneration
      );
    } catch (error) {
      if (!isAlreadyDisconnectedError(error)) throw error;
    }
  }

  private retireNotRequiredExpectedPeer(
    nodeRoutingId: string,
    advertisedEndpoint?: string
  ): void {
    const expected = this.expectedPeers.get(nodeRoutingId);
    const endpoint = expected?.endpoint
      ?? (
        advertisedEndpoint !== undefined
        && this.endpointOnlyPeers.has(advertisedEndpoint)
          ? advertisedEndpoint
          : undefined
      );
    if (endpoint !== undefined) {
      this.requireStarted().disconnect(endpoint);
      this.endpointOnlyPeers.delete(endpoint);
      this.onPeerNotRequired?.(nodeRoutingId, endpoint);
    }
    this.expectedPeers.delete(nodeRoutingId);
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
    if (this.monitorConnectionStates.get(peer.descriptor.nodeRoutingId) === peer.connectionId) {
      this.monitorConnectionStates.set(peer.descriptor.nodeRoutingId, null);
    }
    this.topology.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
    this.liveness.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
    this.onPeerDisconnected?.(
      peer.descriptor.nodeRoutingId,
      peer.descriptor.advertisedEndpoint,
      peer.descriptor.lifecycleGeneration
    );
  }

  private observeMonitorEvent(event: ZLinkRawMonitorRecord): void {
    const nodeRoutingId = event.routingId
      ?? this.expectedPeerRoutingId(event.remoteAddress);
    if (nodeRoutingId === undefined) return;
    const connectionId = monitorConnectionId(event, nodeRoutingId);
    if (event.event === MONITOR_CONNECTION_READY && isConnectionReadyEdge(event)) {
      if (!this.acceptsExpectedMonitorEndpoint(nodeRoutingId, event)) return;
      // The event is queued for admission. Updating monitor state here would
      // allow a late stale READY to overwrite the admitted replacement.
      return;
    }
    if (event.event === MONITOR_DISCONNECTED
      && this.monitorConnectionStates.get(nodeRoutingId) === connectionId
      && this.topology.peer(nodeRoutingId)?.connectionId === connectionId
      && (event.transportPairId === undefined
        || event.transportPairGeneration === undefined
        || event.transportPairId === 0n
        || event.transportPairGeneration === 0n)) {
      this.monitorConnectionStates.set(nodeRoutingId, null);
    }
  }

  private requireStarted(): ZLinkRawRouterPort {
    if (this.router === undefined) throw new Error('Raw service runtime is not started.');
    return this.router;
  }

  private async send(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[]
  ): Promise<boolean> {
    try {
      await this.requireStarted().send(targetNodeRoutingId, parts);
      return true;
    } catch {
      return false;
    }
  }

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
  return (error as { readonly nativeErrno?: unknown }).nativeErrno === 2;
}

function monitorConnectionId(
  event: ZLinkRawMonitorRecord,
  resolvedNodeRoutingId = event.routingId ?? ''
): string {
  if (
    event.transportPairId !== undefined
    && event.transportPairGeneration !== undefined
    && event.transportPairId !== 0n
    && event.transportPairGeneration !== 0n
  ) {
    return JSON.stringify([
      resolvedNodeRoutingId,
      'transport-pair',
      event.transportPairId.toString(),
      event.transportPairGeneration.toString()
    ]);
  }
  if (event.connectionId !== undefined && event.connectionId !== 0n) {
    return JSON.stringify([
      resolvedNodeRoutingId,
      'connection',
      event.connectionId.toString()
    ]);
  }
  return JSON.stringify([
    // Monitor `value` is event-specific: CONNECTION_READY reports the
    // socket's ready count and DISCONNECTED reports its reason. It is not a
    // connection identity. The public monitor record has no native
    // connection id, so use the stable routing and endpoint tuple instead.
    resolvedNodeRoutingId,
    event.localAddress,
    event.remoteAddress
  ]);
}

function sameMonitorConnection(
  left: ZLinkRawMonitorRecord,
  right: ZLinkRawMonitorRecord
): boolean {
  return monitorConnectionId(left) === monitorConnectionId(right)
    && left.remoteAddress === right.remoteAddress;
}

function isConnectionReadyEdge(event: ZLinkRawMonitorRecord): boolean {
  return event.flags === undefined
    ? event.value > 0
    : (event.flags & MONITOR_CONNECTION_READY_EDGE) !== 0;
}

function admissionReason(result: PeerAdmissionResult): number {
  switch (result) {
    case 'meshMismatch':
      return 2;
    case 'staleDescriptor':
      return 7;
    case 'invalidDescriptor':
      return 11;
    case 'notRequired':
      return 4;
    case 'admitted':
      return 1;
  }
}
