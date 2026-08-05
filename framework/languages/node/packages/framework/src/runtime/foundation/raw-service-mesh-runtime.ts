import { randomUUID } from 'node:crypto';
import type {
  ZLinkRawHostPort,
  ZLinkRawMonitorRecord,
  ZLinkRawMonitorPort,
  ZLinkRawRouterPort
} from '../backend/node/node-raw-binding-port';
import { ZLinkNodeRawBindingPort } from '../backend/node/node-raw-binding-port';
import { OperationRegistry, type PendingOperation } from './operation-registry';
import { ServiceLivenessRegistry, type ServiceLivenessTick } from './service-liveness-registry';
import { ServiceMailbox, type ServiceMailboxLimits, type ServiceMailboxRecord } from './service-mailbox';
import {
  ServiceTopologyRegistry,
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

export type RawServicePumpResult =
  | 'noData'
  | 'infrastructure'
  | 'application'
  | 'protocolError';

export interface RawServiceRequestResult {
  readonly terminalResult: number;
  readonly failureCode: number;
  readonly payload?: ServiceApplicationPayload;
}

export interface RawServiceIngressRecord {
  readonly command: number;
  readonly flags: number;
  readonly sourceRoutingId: string;
  readonly sourceRoute?: Uint8Array;
  readonly requestSequence?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];
}

export type RawServiceIngressHandler = (
  record: RawServiceIngressRecord
) => RawServicePumpResult | undefined;

export interface RawServiceMeshRuntimeOptions {
  readonly descriptor: ServiceNodeDescriptor;
  readonly mailbox?: Partial<ServiceMailboxLimits>;
  readonly probeIntervalMs?: number;
  readonly peerTimeoutMs?: number;
  readonly bindingPort?: { createHost(): ZLinkRawHostPort };
  readonly onPeerNotRequired?: (
    nodeRoutingId: string,
    endpoint: string
  ) => void;
  readonly onInboundMessageDropped?: (
    surface: 'node' | 'channel',
    messageKind: 'send',
    reason: 'backpressure'
  ) => void;
}

const DEFAULT_MAILBOX_LIMITS: ServiceMailboxLimits = {
  applicationMessages: 4_096,
  applicationBytes: 64 * 1024 * 1024,
  infrastructureMessages: 1_024,
  infrastructureBytes: 8 * 1024 * 1024
};
const MONITOR_DISCONNECTED = 0x0200;
const MONITOR_CONNECTION_READY = 0x1000;
const ENDPOINT_BILATERAL_GRACE_MS = 100;
const MAX_COMPLETION_CONTROL_BYTES = 64 * 1024;
const COMPLETION_CONTROL_COMMANDS = new Set<number>([
  M6aServiceWireCommand.hello,
  M6aServiceWireCommand.admit,
  M6aServiceWireCommand.reject,
  M6aServiceWireCommand.update,
  M6aServiceWireCommand.livenessProbe,
  M6aServiceWireCommand.livenessAck,
  // Relocation coordination and reply recovery commands. Object messages,
  // lifecycle callbacks and normal request/reply commands are deliberately
  // absent from this allowlist.
  30, 31, 32, 33, 34, 35,
  40, 41, 42, 43, 44, 45, 46
]);

type PhysicalConnectionDirection = 'inbound' | 'outbound' | 'unknown';

interface PhysicalConnectionCandidate {
  readonly connectionId: string;
  readonly direction: PhysicalConnectionDirection;
  readonly discriminator: string;
  readonly localAddress: string;
  readonly remoteAddress: string;
}

const livenessCodec = createServiceWireCodec({
  magic: [0x5a, 0x4d],
  major: 1,
  commands: M6aServiceWireCommand
});

/**
 * RouteMesh M6A runtime built only on the public raw binding package.
 * It owns protocol, admission, mailbox, completion and liveness state.
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
  private readonly passiveBilateralPeers = new Set<string>();
  private readonly inboundPeerRoutes = new Set<string>();
  private readonly upgradingEndpointPeers = new Set<string>();
  private readonly pendingEndpointUpgrades = new Map<string, {
    readonly endpoint: string;
    readonly deadlineMs: number;
  }>();
  private readonly endpointUpgradePreviousConnections = new Map<string, string>();
  private readonly connectionCandidates = new Map<
    string,
    Map<string, PhysicalConnectionCandidate>
  >();
  private readonly unresolvedConnectionCandidates: PhysicalConnectionCandidate[] = [];
  private readonly connectionIds = new Map<string, string>();
  private monitorEvents: ZLinkRawMonitorRecord[] = [];
  private readonly bindingPort: { createHost(): ZLinkRawHostPort };
  private readonly onPeerNotRequired?: RawServiceMeshRuntimeOptions['onPeerNotRequired'];
  private readonly onInboundMessageDropped?: RawServiceMeshRuntimeOptions['onInboundMessageDropped'];
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
    this.mailbox = new ServiceMailbox({ ...DEFAULT_MAILBOX_LIMITS, ...options.mailbox });
    this.liveness = new ServiceLivenessRegistry(options.probeIntervalMs, options.peerTimeoutMs);
    this.bindingPort = options.bindingPort ?? new ZLinkNodeRawBindingPort();
    this.onPeerNotRequired = options.onPeerNotRequired;
    this.onInboundMessageDropped = options.onInboundMessageDropped;
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
      this.monitor = router.monitor(event => this.monitorEvents.push(event));
      router.setCompletionControlHandler?.((sourceRid, parts) => {
        // The binding callback owns Completion progress independently of
        // Application Recv. Process the copied record synchronously so the
        // Framework does not add another payload queue.
        this.processReceived({
          sourceRid,
          sourceRoute: Buffer.alloc(0),
          parts
        }, performance.now(), true);
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

  announcePeer(nodeRoutingId: string): boolean {
    if (!this.expectedPeers.has(nodeRoutingId)) return false;
    return this.trySend(
      nodeRoutingId,
      [encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())]
    );
  }

  isPeerRouteReady(nodeRoutingId: string, lifecycleGeneration?: bigint): boolean {
    if (this.upgradingEndpointPeers.has(nodeRoutingId)) return false;
    const peer = this.topology.peer(nodeRoutingId);
    return peer !== undefined
      && (lifecycleGeneration === undefined
        || peer.descriptor.lifecycleGeneration === lifecycleGeneration)
      && this.liveness.isReady(nodeRoutingId, peer.connectionId);
  }

  announceExpectedPeers(): number {
    this.advanceEndpointUpgrades(Date.now());
    let accepted = 0;
    for (const nodeRoutingId of this.expectedPeers.keys()) {
      // Admission is a one-time fence for the current physical connection.
      // Re-sending Hello after the peer is admitted would re-run admission on
      // every poll and reset the liveness record before application traffic
      // can use the route. A disconnected peer is removed by monitor handling
      // and remains eligible for the next admission attempt.
      if (this.topology.peer(nodeRoutingId) !== undefined) continue;
      if (this.announcePeer(nodeRoutingId)) accepted++;
    }
    return accepted;
  }

  updateLocalWeights(options: {
    readonly placementWeight?: number;
    readonly channelName?: string;
    readonly channelWeight?: number;
  }): void {
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
    this.announceExpectedPeers();
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

  sendToNode(targetNodeRoutingId: string, payload: ServiceApplicationPayload): boolean {
    return this.trySend(
      targetNodeRoutingId,
      [encodeNodeSendHeader(), encodeApplicationPayload(payload)]
    );
  }

  sendToChannel(channelName: string, payload: ServiceApplicationPayload): boolean {
    const selected = this.topology.selectChannel(
      channelName,
      peer => this.isLocalOrReadyPeer(peer.descriptor.nodeRoutingId)
    );
    if (selected === undefined) return false;
    if (selected.descriptor.nodeRoutingId === this.descriptor.nodeRoutingId) {
      return this.mailbox.tryEnqueue({
        owner: `channel:${channelName}`,
        domain: 'application',
        parts: [encodeChannelSendHeader(channelName), encodeApplicationPayload(payload)],
        sourceRoutingId: this.descriptor.nodeRoutingId
      });
    }
    return this.trySend(selected.descriptor.nodeRoutingId, [
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

  sendService(targetNodeRoutingId: string, parts: readonly Uint8Array[]): boolean {
    return this.trySend(targetNodeRoutingId, parts);
  }

  /**
   * Sends only the bounded Framework control commands that are safe while
   * Application receive is paused.
   */
  sendCompletionControl(
    targetNodeRoutingId: string,
    parts: readonly Uint8Array[]
  ): boolean {
    if (!validCompletionControlRecord(parts)) return false;
    try {
      return this.requireStarted().trySendCompletionControl?.(
        targetNodeRoutingId,
        parts
      ) ?? false;
    } catch {
      return false;
    }
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

  pumpOne(nowMs = performance.now()): RawServicePumpResult {
    const router = this.requireStarted();
    const received = router.receive(true);
    if (received === undefined) return 'noData';
    return this.processReceived(received, nowMs, false);
  }

  /**
   * Progresses request completions and bounded Framework controls without
   * consuming a message from the Application connection.
   */
  progressCompletion(): number {
    return this.requireStarted().progressCompletion?.() ?? 0;
  }

  private processReceived(
    received: import('../backend/node/node-raw-binding-port').ZLinkRawReceivedRecord,
    nowMs: number,
    completionControl: boolean
  ): RawServicePumpResult {
    if (completionControl && !validCompletionControlRecord(received.parts)) {
      return 'protocolError';
    }
    if (
      received.parts.length === 0
      || (
        received.parts.length === 1
        && received.parts[0]!.byteLength === 0
      )
    ) {
      return this.trySend(
        received.sourceRid,
        [encodeRouteMeshAdmission(M6aServiceWireCommand.hello, this.topology.localDescriptor())]
      )
        ? 'infrastructure'
        : 'protocolError';
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
          this.trySend(received.sourceRid, [encodeReject(3)]);
          return 'infrastructure';
        }
        const connection = this.currentConnectionCandidate(
          received.sourceRid,
          descriptor.advertisedEndpoint
        );
        const result = this.admitPeer(
          descriptor,
          connection,
          nowMs,
          expected
        );
        if (result !== 'admitted') {
          this.trySend(received.sourceRid, [encodeReject(admissionReason(result))]);
          if (result === 'notRequired') {
            this.retireNotRequiredExpectedPeer(
              received.sourceRid,
              descriptor.advertisedEndpoint
            );
          }
          return 'infrastructure';
        }
        if (
          header.command === M6aServiceWireCommand.admit
          && expected !== undefined
        ) {
          const previousConnectionId = this.endpointUpgradePreviousConnections.get(
            descriptor.nodeRoutingId
          );
          if (
            !this.upgradingEndpointPeers.has(descriptor.nodeRoutingId)
            || previousConnectionId === undefined
            || previousConnectionId !== connection.connectionId
          ) {
            this.upgradingEndpointPeers.delete(descriptor.nodeRoutingId);
            this.endpointUpgradePreviousConnections.delete(descriptor.nodeRoutingId);
          }
        }
        this.selectBilateralConnection(descriptor);
        if (header.command === M6aServiceWireCommand.hello) {
          this.trySend(
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
        if (reason === 4) {
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
          const sent = ack !== undefined && this.trySend(received.sourceRid, [livenessCodec.encodeLivenessRecord({
              command: M6aServiceWireCommand.livenessAck,
              probeId: record.probeId
            })]);
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
      const stateful = this.serviceIngress?.({
        command: header.command,
        flags: header.flags,
        sourceRoutingId: received.sourceRid,
        sourceRoute: received.sourceRoute,
        ...(received.reply === undefined ? {} : { reply: received.reply }),
        ...(received.requestSeq === undefined ? {} : { requestSequence: received.requestSeq }),
        parts: received.parts
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
      const accepted = this.mailbox.tryEnqueue({
        owner,
        domain: 'application',
        parts: received.parts,
        sourceRoutingId: received.sourceRid,
        sourceRoute: received.sourceRoute,
        ...(received.reply === undefined ? {} : { reply: received.reply }),
        requestSequence: received.requestSeq,
        ...(correlation === undefined ? {} : { correlation })
      });
      if (accepted) return 'application';
      if (header.command === M6aServiceWireCommand.nodeSend) {
        this.onInboundMessageDropped?.('node', 'send', 'backpressure');
      } else if (header.command === M6aServiceWireCommand.channelSend) {
        this.onInboundMessageDropped?.('channel', 'send', 'backpressure');
      }
      return 'protocolError';
    } catch (error) {
      if (error instanceof ServiceWireProtocolError) return 'protocolError';
      throw error;
    }
  }

  tickLiveness(nowMs = performance.now()): ServiceLivenessTick {
    const result = this.liveness.tick(nowMs);
    this.requireStarted();
    for (const probe of result.probes) {
      this.trySend(probe.nodeRoutingId, [livenessCodec.encodeLivenessRecord({
        command: M6aServiceWireCommand.livenessProbe,
        probeId: probe.probeId
      })]);
    }
    for (const nodeRoutingId of result.timedOutNodes) {
      const peer = this.topology.peer(nodeRoutingId);
      if (peer !== undefined) this.topology.disconnect(nodeRoutingId, peer.connectionId);
    }
    return result;
  }

  drainMonitorEvents(nowMs = performance.now()): number {
    let handled = 0;
    // Detach the current batch so monitor callbacks that run while an event is
    // being handled append to the next batch without copying or reindexing it.
    const events = this.monitorEvents;
    this.monitorEvents = [];
    for (const event of events) {
      handled++;
      const nodeRoutingId = event.routingId
        ?? this.expectedPeerRoutingId(event.remoteAddress);
      if (event.event === MONITOR_CONNECTION_READY) {
        if (nodeRoutingId === undefined) {
          this.unresolvedConnectionCandidates.push(
            this.createUnresolvedConnectionCandidate(event)
          );
          continue;
        }
        const candidate = this.createConnectionCandidate(nodeRoutingId, event);
        let candidates = this.connectionCandidates.get(nodeRoutingId);
        if (candidates === undefined) {
          candidates = new Map();
          this.connectionCandidates.set(nodeRoutingId, candidates);
        }
        candidates.set(candidate.connectionId, candidate);
        this.connectionIds.set(nodeRoutingId, candidate.connectionId);
        const admitted = this.topology.peer(nodeRoutingId);
        if (admitted !== undefined) {
          this.admitPeer(
            admitted.descriptor,
            candidate,
            nowMs,
            this.expectedPeers.get(nodeRoutingId)
          );
        }
        if (event.localAddress === this.topology.localDescriptor().advertisedEndpoint) {
          this.inboundPeerRoutes.add(nodeRoutingId);
          this.advanceEndpointUpgrade(nodeRoutingId, Date.now());
        }
        this.announcePeer(nodeRoutingId);
      } else if (event.event === MONITOR_DISCONNECTED && nodeRoutingId !== undefined) {
        const peer = this.topology.peer(nodeRoutingId);
        const disconnectedId = monitorConnectionId(event);
        const candidate = this.connectionCandidates.get(nodeRoutingId)?.get(disconnectedId);
        this.removeConnectionCandidate(nodeRoutingId, disconnectedId);
        if (
          this.passiveBilateralPeers.has(nodeRoutingId)
          && peer !== undefined
          && candidate?.direction === 'outbound'
          && event.remoteAddress === peer.descriptor.advertisedEndpoint
        ) {
          // The larger RID retires only its outbound half after learning the
          // peer RID. The smaller RID's inbound connection remains canonical.
          continue;
        }
        if (peer !== undefined && peer.connectionId === disconnectedId) {
          this.removePeer(peer);
        }
      } else if (event.event === MONITOR_DISCONNECTED) {
        const disconnectedId = monitorConnectionId(event);
        const index = this.unresolvedConnectionCandidates.findIndex(
          candidate => candidate.connectionId === disconnectedId
        );
        if (index >= 0) this.unresolvedConnectionCandidates.splice(index, 1);
      }
    }
    void nowMs;
    return handled;
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
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
    const pending = this.operations.reserve(timeoutMs);
    const correlation = this.nextCorrelation++;
    const header = channelName === undefined
      ? encodeNodeRequestHeader(correlation)
      : encodeChannelRequestHeader(correlation, channelName);
    if (targetNodeRoutingId === this.descriptor.nodeRoutingId) {
      const accepted = this.mailbox.tryEnqueue({
        owner: channelName === undefined
          ? `node:${this.descriptor.nodeRoutingId}`
          : `channel:${channelName}`,
        domain: 'application',
        parts: [header, encodeApplicationPayload(payload)],
        sourceRoutingId: this.descriptor.nodeRoutingId,
        correlation,
        localReply: (terminalResult, failureCode, reply) =>
          this.operations.complete(pending.id, {
            terminalResult,
            failureCode,
            ...(reply === undefined ? {} : { payload: reply })
          })
      });
      if (!accepted) {
        this.operations.complete(pending.id, {
          terminalResult: 109,
          failureCode: 0
        });
      }
      return pending;
    }
    void this.requireStarted().request(
      targetNodeRoutingId,
      [header, encodeApplicationPayload(payload)],
      timeoutMs
    ).then(
      parts => {
        try {
          if (parts.length < 1 || parts.length > 2) throw new ServiceWireProtocolError('Invalid reply parts.');
          const reply = decodeReplyHeader(parts[0]!);
          if (reply.correlation !== correlation) throw new ServiceWireProtocolError('Reply correlation mismatch.');
          if (reply.tail.byteLength !== 0) {
            throw new ServiceWireProtocolError('Generic node/channel reply carries an operation-specific tail.');
          }
          if (reply.terminalResult === 0 && parts.length !== 2) {
            throw new ServiceWireProtocolError('Successful reply omits its payload.');
          }
          if (reply.terminalResult !== 0 && parts.length !== 1) {
            throw new ServiceWireProtocolError('Failed reply carries a payload.');
          }
          const result: RawServiceRequestResult = {
            terminalResult: reply.terminalResult,
            failureCode: reply.failureCode
          };
          this.operations.complete(
            pending.id,
            reply.terminalResult === 0
              ? { ...result, payload: decodeApplicationPayload(parts[1]!) }
              : result
          );
        } catch (error) {
          this.operations.fail(pending.id, error);
        }
      },
      error => this.operations.fail(pending.id, error)
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
    }
    return result;
  }

  private currentConnectionCandidate(
    nodeRoutingId: string,
    advertisedEndpoint: string
  ): PhysicalConnectionCandidate {
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
      const initiator = endpointOnly
        && localRid.localeCompare(nodeRoutingId) > 0
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
      connectionId: monitorConnectionId(event),
      direction,
      discriminator: initiator === undefined
        ? `unknown:${monitorConnectionId(event)}`
        : `initiator:${initiator}`,
      localAddress: event.localAddress,
      remoteAddress: event.remoteAddress
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
      remoteAddress: event.remoteAddress
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
    this.upgradingEndpointPeers.add(descriptor.nodeRoutingId);
    const current = this.topology.peer(descriptor.nodeRoutingId);
    if (current !== undefined) {
      this.endpointUpgradePreviousConnections.set(
        descriptor.nodeRoutingId,
        current.connectionId
      );
    }
    this.pendingEndpointUpgrades.set(descriptor.nodeRoutingId, {
      endpoint,
      deadlineMs: Date.now() + ENDPOINT_BILATERAL_GRACE_MS
    });
    const localRid = this.topology.localDescriptor().nodeRoutingId;
    if (
      localRid.localeCompare(descriptor.nodeRoutingId) > 0
      && this.topology.peer(descriptor.nodeRoutingId)?.connectionDiscriminator
        === `initiator:${descriptor.nodeRoutingId}`
    ) {
      // An endpoint-only route has no physical direction when the monitor
      // event is unavailable. Keep the established route and use the lower
      // RID as the deterministic logical initiator until a physical
      // candidate is observed.
      this.pendingEndpointUpgrades.delete(descriptor.nodeRoutingId);
      this.passiveBilateralPeers.add(descriptor.nodeRoutingId);
      this.upgradingEndpointPeers.delete(descriptor.nodeRoutingId);
      this.endpointUpgradePreviousConnections.delete(descriptor.nodeRoutingId);
      return;
    }
    if (localRid.localeCompare(descriptor.nodeRoutingId) <= 0) {
      // The endpoint-only pipe already carries the admission exchange. Once
      // the peer RID is known, bind that existing pipe to the expected peer
      // state instead of tearing it down and racing a replacement pipe.
      this.pendingEndpointUpgrades.delete(descriptor.nodeRoutingId);
      this.upgradingEndpointPeers.delete(descriptor.nodeRoutingId);
      this.endpointUpgradePreviousConnections.delete(descriptor.nodeRoutingId);
    }
  }

  private advanceEndpointUpgrades(nowMs: number): void {
    for (const nodeRoutingId of this.pendingEndpointUpgrades.keys()) {
      this.advanceEndpointUpgrade(nodeRoutingId, nowMs);
    }
  }

  private advanceEndpointUpgrade(nodeRoutingId: string, nowMs: number): void {
    const pending = this.pendingEndpointUpgrades.get(nodeRoutingId);
    if (pending === undefined) return;
    const localRid = this.topology.localDescriptor().nodeRoutingId;
    if (
      localRid.localeCompare(nodeRoutingId) > 0
      && this.inboundPeerRoutes.has(nodeRoutingId)
    ) {
      this.pendingEndpointUpgrades.delete(nodeRoutingId);
      this.requireStarted().disconnect(pending.endpoint);
      this.passiveBilateralPeers.add(nodeRoutingId);
      this.upgradingEndpointPeers.delete(nodeRoutingId);
      this.endpointUpgradePreviousConnections.delete(nodeRoutingId);
      return;
    }
    if (nowMs < pending.deadlineMs) return;
    this.activateEndpointUpgrade(nodeRoutingId, pending.endpoint);
  }

  private activateEndpointUpgrade(nodeRoutingId: string, endpoint: string): void {
    this.pendingEndpointUpgrades.delete(nodeRoutingId);
    // Replace the endpoint-only probe rather than keeping two connections.
    // Request/reply sequencing is scoped to one physical route.
    const current = this.topology.peer(nodeRoutingId);
    if (current !== undefined) this.removePeer(current);
    const currentConnectionId = this.connectionIds.get(nodeRoutingId);
    if (currentConnectionId !== undefined) {
      this.removeConnectionCandidate(nodeRoutingId, currentConnectionId);
    }
    this.requireStarted().disconnect(endpoint);
    this.requireStarted().connectToRoutingId(nodeRoutingId, endpoint);
  }

  private removePeer(peer: AdmittedServicePeer): void {
    this.topology.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
    this.liveness.disconnect(peer.descriptor.nodeRoutingId, peer.connectionId);
  }

  private requireStarted(): ZLinkRawRouterPort {
    if (this.router === undefined) throw new Error('Raw service runtime is not started.');
    return this.router;
  }

  private trySend(targetNodeRoutingId: string, parts: readonly Uint8Array[]): boolean {
    try {
      const router = this.requireStarted();
      const command = completionControlCommand(parts);
      if (
        command !== undefined
        && command >= M6aServiceWireCommand.livenessProbe
        && command <= M6aServiceWireCommand.livenessAck
        && router.trySendCompletionControl !== undefined
      ) {
        return router.trySendCompletionControl(targetNodeRoutingId, parts);
      }
      return router.send(targetNodeRoutingId, parts, true);
    } catch {
      return false;
    }
  }
}

function validCompletionControlRecord(parts: readonly Uint8Array[]): boolean {
  return completionControlCommand(parts) !== undefined
    && parts.reduce((total, part) => total + part.byteLength, 0)
      <= MAX_COMPLETION_CONTROL_BYTES;
}

function isAlreadyDisconnectedError(error: unknown): boolean {
  if (typeof error !== 'object' || error === null || !('nativeErrno' in error)) {
    return false;
  }
  return (error as { readonly nativeErrno?: unknown }).nativeErrno === 2;
}

function completionControlCommand(
  parts: readonly Uint8Array[]
): number | undefined {
  if (parts.length !== 1 || parts[0]!.byteLength < 5) return undefined;
  try {
    const header = decodeHeader(parts[0]!);
    const validFlags = header.command === 30
      ? header.flags === 8
      : header.flags === 0;
    return validFlags && COMPLETION_CONTROL_COMMANDS.has(header.command)
      ? header.command
      : undefined;
  } catch {
    return undefined;
  }
}

function monitorConnectionId(event: ZLinkRawMonitorRecord): string {
  return JSON.stringify([
    event.value,
    event.localAddress,
    event.remoteAddress
  ]);
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
