import {
  Message,
  Received,
  RecvFlags,
  RoutingId as BindingRoutingId,
  createContext,
  createDealerSocket,
  createRouterSocket,
  ReceiveFlowState,
  type Context,
  type DealerSocket,
  type MonitorEvent,
  type MonitorSocket,
  type ReplyOperation,
  type ReplyToken,
  type ReplySubmitOperation,
  type RequestOperation,
  type RequestSubmitOperation,
  type RouterSocket,
  type Socket
} from '@zlink-systems/zlink';
import {
  translateBindingResultError,
  bindingRequestReply,
  submitBindingAsyncSend,
  submitBindingSend
} from './node-backend-adapter-support';
import { disconnectSocketPeer, isEndpointCloseIgnorableError } from './node-socket-backend-adapter';
import { SubmitResult } from '../runtime-values';
import { ZLinkNodeEventLoopPoller } from './node-event-loop-poller';
import { ZLinkBufferMessage } from '../runtime-message';
import type {
  ZLinkRawBindingPort,
  ZLinkRawDealerPort,
  ZLinkRawHostPort,
  ZLinkRawMonitorPort,
  ZLinkRawMonitorRecord,
  ZLinkRawReceivedRecord,
  ZLinkRawRouterPort,
  ZLinkRawRouterRoute,
  ZLinkRawSocketPort
} from '../raw-binding-port';
export type {
  ZLinkRawBindingPort,
  ZLinkRawDealerPort,
  ZLinkRawHostPort,
  ZLinkRawMonitorPort,
  ZLinkRawMonitorRecord,
  ZLinkRawReceivedRecord,
  ZLinkRawRouterPort,
  ZLinkRawRouterRoute,
  ZLinkRawSocketPort
} from '../raw-binding-port';

export class ZLinkNodeRawBindingPort implements ZLinkRawBindingPort {
  constructor(
    private readonly sharedContext?: Context,
    private readonly routerReceiveTimeoutMs?: number
  ) {}

  createHost(): ZLinkRawHostPort {
    return new NodeRawHostPort(
      this.sharedContext ?? createContext(),
      this.sharedContext === undefined,
      this.routerReceiveTimeoutMs
    );
  }
}

class NodeRawHostPort implements ZLinkRawHostPort {
  private readonly resources: Array<{ close(): void }> = [];
  private state: 'open' | 'stopping' | 'closed' = 'open';

  constructor(
    private readonly context: Context,
    private readonly ownsContext: boolean,
    private readonly routerReceiveTimeoutMs?: number
  ) {}

  createRouter(): ZLinkRawRouterPort {
    this.requireOpen();
    return this.own(
      new NodeRawRouterPort(createRouterSocket(this.context), this.routerReceiveTimeoutMs)
    );
  }

  createDealer(): ZLinkRawDealerPort {
    this.requireOpen();
    return this.own(new NodeRawDealerPort(createDealerSocket(this.context)));
  }

  shutdown(): void {
    if (this.state !== 'open') return;
    this.state = 'stopping';
    if (this.ownsContext) this.context.shutdown();
  }

  close(): void {
    if (this.state === 'closed') return;
    this.state = 'stopping';
    const failures: unknown[] = [];
    for (const resource of [...this.resources].reverse()) {
      try {
        resource.close();
        this.resources.splice(this.resources.indexOf(resource), 1);
      } catch (error) {
        failures.push(error);
      }
    }
    if (this.ownsContext && failures.length === 0) {
      try {
        this.context.close();
      } catch (error) {
        failures.push(error);
      }
    }
    if (failures.length > 0) {
      throw new AggregateError(failures, 'Raw binding host cleanup failed.');
    }
    this.state = 'closed';
  }

  private own<T extends { close(): void }>(resource: T): T {
    this.resources.push(resource);
    return resource;
  }

  private requireOpen(): void {
    if (this.state !== 'open') {
      throw new Error('Raw binding host no longer accepts new resources.');
    }
  }
}

abstract class NodeRawSocketPort<TSocket extends Socket> implements ZLinkRawSocketPort {
  protected readonly received = new Received();
  private readonly endpoints = new Set<string>();
  private readonly monitors = new Set<NodeRawMonitorPort>();
  private readonly eventLoopPoller: ZLinkNodeEventLoopPoller;
  private closed = false;

  protected constructor(
    protected readonly socket: TSocket,
    pollRoute = false
  ) {
    this.eventLoopPoller = new ZLinkNodeEventLoopPoller(socket, true, () => {}, pollRoute);
  }

  setReadableHandler(handler: (receiveReady: boolean, routeReady: boolean) => void): void {
    this.requireOpen();
    this.eventLoopPoller.setReadableHandler(handler);
  }

  bind(endpoint: string): void {
    this.requireOpen();
    this.socket.bind(endpoint);
    this.endpoints.add(`bind\0${endpoint}`);
  }

  unbind(endpoint: string): void {
    this.requireOpen();
    this.socket.unbind(endpoint);
    this.endpoints.delete(`bind\0${endpoint}`);
  }

  connect(endpoint: string): void {
    this.requireOpen();
    connectable(this.socket).connect(endpoint);
    this.endpoints.add(`connect\0${endpoint}`);
  }

  disconnect(endpoint: string): void {
    this.requireOpen();
    connectable(this.socket).disconnect(endpoint);
    this.endpoints.delete(`connect\0${endpoint}`);
  }

  monitor(): ZLinkRawMonitorPort {
    this.requireOpen();
    const nativeMonitor = this.socket.monitorOpen();
    const port = new NodeRawMonitorPort(nativeMonitor, () => this.monitors.delete(port));
    this.monitors.add(port);
    return port;
  }

  close(): void {
    if (this.closed) return;
    const failures: unknown[] = [];
    try {
      this.eventLoopPoller.dispose();
    } catch (error) {
      failures.push(error);
    }
    for (const monitor of [...this.monitors].reverse()) {
      try {
        monitor.close();
      } catch (error) {
        failures.push(error);
      }
    }
    for (const value of [...this.endpoints].reverse()) {
      const separator = value.indexOf('\0');
      const kind = value.slice(0, separator);
      const endpoint = value.slice(separator + 1);
      try {
        if (kind === 'bind') this.socket.unbind(endpoint);
        else connectable(this.socket).disconnect(endpoint);
      } catch (error) {
        if (isEndpointCloseIgnorableError(error)) continue;
        failures.push(error);
      }
    }
    if (failures.length > 0) {
      throw new AggregateError(failures, 'Raw socket cleanup failed.');
    }
    this.endpoints.clear();
    try {
      this.received.close();
      this.socket.close();
    } catch (error) {
      throw new AggregateError([error], 'Raw socket cleanup failed.');
    }
    this.closed = true;
  }

  protected requireOpen(): void {
    if (this.closed) throw new Error('Raw socket is closed.');
  }
}

class NodeRawRouterPort extends NodeRawSocketPort<RouterSocket> implements ZLinkRawRouterPort {
  private nextIncomingRequestSequence = 1n;
  private readonly routingIds = new Map<string, BindingRoutingId>();
  private readonly nextRequestSequence = () => {
    const current = this.nextIncomingRequestSequence;
    this.nextIncomingRequestSequence =
      current === BigInt(Number.MAX_SAFE_INTEGER) ? 1n : current + 1n;
    return current;
  };
  private readonly replyParts = (
    targetRid: BindingRoutingId,
    replyToken: ReplyToken,
    parts: readonly Uint8Array[]
  ) => {
    // The receive record keeps the binding RoutingId it was delivered with,
    // so the reply addresses that route without rebuilding the identity.
    appendReplyParts(this.socket.reply(targetRid, replyToken), parts).submit();
  };

  private preparedRoutingId(routingId: string | Uint8Array): BindingRoutingId {
    if (typeof routingId !== 'string') return bindingRoutingId(routingId);
    let prepared = this.routingIds.get(routingId);
    if (prepared === undefined)
      this.routingIds.set(routingId, (prepared = bindingRoutingId(routingId)));
    return prepared;
  }

  override close(): void {
    try {
      super.close();
    } finally {
      this.routingIds.clear();
    }
  }

  setReceiveFlowState(state: 'running' | 'paused'): void {
    this.requireOpen();
    this.socket.setReceiveFlowState(
      state === 'paused' ? ReceiveFlowState.PAUSED : ReceiveFlowState.RUNNING
    );
  }

  disconnectRid(routingId: string): void {
    this.requireOpen();
    disconnectSocketPeer(this.socket, this.preparedRoutingId(routingId));
  }

  constructor(socket: RouterSocket, receiveTimeoutMs?: number) {
    super(socket, true);
    socket.options.handover = true;
    socket.options.mandatory = true;
    socket.options.probe = true;
    if (receiveTimeoutMs !== undefined) {
      socket.options.recvTimeout = receiveTimeoutMs;
    }
  }

  localEndpoint(): string {
    this.requireOpen();
    return this.socket.options.lastEndpoint;
  }

  setRoutingId(routingId: string): void {
    this.requireOpen();
    this.socket.setRoutingId(this.preparedRoutingId(routingId));
  }

  connectToRoutingId(routingId: string, endpoint: string): void {
    this.requireOpen();
    this.socket.options.setConnectRoutingId(this.preparedRoutingId(routingId));
    this.connect(endpoint);
  }

  send(targetRid: string, parts: readonly Uint8Array[]): Promise<void> {
    this.requireOpen();
    requireParts(parts);
    return submitBindingAsyncSend(this.socket.send(this.preparedRoutingId(targetRid)), parts);
  }

  submitSend(targetRid: string, parts: readonly Uint8Array[]) {
    this.requireOpen();
    requireParts(parts);
    return submitBindingSend(this.socket.send(this.preparedRoutingId(targetRid)), parts);
  }

  request<T>(
    targetRid: string,
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onReply: (parts: readonly Buffer[]) => T,
    onFailed: (error: unknown) => T,
    recordReceived?: () => void,
    onAdmitted?: () => void
  ): Promise<T> {
    let submission: ReturnType<RequestSubmitOperation['submit']>;
    const deadlineMs = performance.now() + timeoutMs;
    try {
      this.requireOpen();
      submission = appendRequestParts(this.socket.request(this.preparedRoutingId(targetRid)), parts)
        .timeout(timeoutMs)
        .submit();
    } catch (error) {
      throw translateBindingResultError(error, 'submit');
    }
    return requestReply(submission, deadlineMs, onReply, onFailed, recordReceived, onAdmitted);
  }

  routesSnapshot(): readonly ZLinkRawRouterRoute[] {
    this.requireOpen();
    return this.socket.routesSnapshot().map((route) => ({
      routingId: route.routingId.toString(),
      routeGeneration: route.routeGeneration
    }));
  }

  receive(dontWait = false): ZLinkRawReceivedRecord | undefined {
    this.requireOpen();
    return receiveRecord(
      this.socket,
      this.received,
      dontWait,
      this.nextRequestSequence,
      this.replyParts
    );
  }
}

class NodeRawDealerPort extends NodeRawSocketPort<DealerSocket> implements ZLinkRawDealerPort {
  constructor(socket: DealerSocket) {
    super(socket);
  }

  setRoutingId(routingId: string): void {
    this.requireOpen();
    this.socket.setRoutingId(bindingRoutingId(routingId));
  }

  send(parts: readonly Uint8Array[]): Promise<void> {
    this.requireOpen();
    requireParts(parts);
    return submitBindingAsyncSend(this.socket.send(), parts);
  }

  request(
    parts: readonly Uint8Array[],
    timeoutMs: number,
    onAdmitted?: () => void
  ): Promise<readonly Buffer[]> {
    let submission: ReturnType<RequestSubmitOperation['submit']>;
    const deadlineMs = performance.now() + timeoutMs;
    try {
      this.requireOpen();
      submission = appendRequestParts(this.socket.request(), parts).timeout(timeoutMs).submit();
      return requestReply(
        submission,
        deadlineMs,
        (parts) => parts,
        rethrowCompletion,
        undefined,
        onAdmitted
      );
    } catch (error) {
      return Promise.reject(translateBindingResultError(error, 'submit'));
    }
  }

  receive(dontWait = false): ZLinkRawReceivedRecord | undefined {
    this.requireOpen();
    return receiveRecord(this.socket, this.received, dontWait);
  }
}

class NodeRawMonitorPort implements ZLinkRawMonitorPort {
  private closed = false;

  constructor(
    private readonly monitor: MonitorSocket,
    private readonly release: () => void
  ) {}

  statusReady(): boolean {
    if (this.closed) throw new Error('Raw monitor is closed.');
    return this.monitor.status().isReady();
  }

  drain(handler: (event: ZLinkRawMonitorRecord) => void): number {
    if (this.closed) throw new Error('Raw monitor is closed.');
    let count = 0;
    for (;;) {
      const event = this.monitor.recv(RecvFlags.DontWait);
      if (event === null) return count;
      handler(copyMonitorEvent(event));
      count += 1;
    }
  }

  close(): void {
    if (this.closed) return;
    this.monitor.close();
    this.closed = true;
    this.release();
  }
}

function appendRequestParts(
  operation: RequestOperation,
  parts: readonly Uint8Array[]
): RequestSubmitOperation {
  requireParts(parts);
  let next = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index += 1) {
    next = next.message(parts[index]);
  }
  return next;
}

function appendReplyParts(
  operation: ReplyOperation,
  parts: readonly Uint8Array[]
): ReplySubmitOperation {
  requireParts(parts);
  let next = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index += 1) {
    next = next.message(parts[index]);
  }
  return next;
}

function requireParts(parts: readonly Uint8Array[]): [Uint8Array, ...Uint8Array[]] {
  if (parts.length === 0)
    throw new TypeError('Raw multipart operation requires at least one part.');
  return parts as [Uint8Array, ...Uint8Array[]];
}

type RawReceivedReply = (
  targetRid: BindingRoutingId,
  replyToken: ReplyToken,
  parts: readonly Uint8Array[]
) => void;

const EMPTY_SOURCE_ROUTE = Buffer.alloc(0);

/**
 * One received binding record. The payload Buffers are the binding's own
 * `data()` storage, retained under the runtime payload policy. The socket owns
 * the reusable binding envelope; snapshots survive its next receive or close.
 */
class NodeRawReceivedRecord implements ZLinkRawReceivedRecord {
  readonly sourceRid: string;
  readonly sourceRoute: Uint8Array;
  readonly routeGeneration: bigint;
  readonly requestSeq?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];

  constructor(
    received: Received,
    nextRequestSequence: (() => bigint) | undefined,
    reply: RawReceivedReply | undefined
  ) {
    const routingId = received.routingId;
    this.sourceRid = routingId?.toString() ?? '';
    this.sourceRoute = routingId?.toBytes() ?? EMPTY_SOURCE_ROUTE;
    this.routeGeneration = received.routeGeneration;
    const replyToken = received.replyToken;
    if (replyToken !== null) {
      this.requestSeq = nextRequestSequence?.();
      if (reply === undefined) {
        const operation = received.reply();
        this.reply = (parts) => appendReplyParts(operation, parts).submit();
      } else {
        const target = routingId ?? bindingRoutingId(EMPTY_SOURCE_ROUTE);
        this.reply = (parts) => reply(target, replyToken, parts);
      }
    }
    const parts = received.parts;
    const retained = new Array<Buffer>(parts.length);
    for (let index = 0; index < parts.length; index++)
      retained[index] = ZLinkBufferMessage.retainPayload(parts[index]!);
    this.parts = retained;
  }

  close(): void {
    // Reading each binding payload above retained JS-owned storage and released
    // its native frame. The socket clears its reusable envelope; these record
    // snapshots need no native cleanup.
  }
}

function receiveRecord(
  socket: Pick<RouterSocket | DealerSocket, 'recv'>,
  received: Received,
  dontWait: boolean,
  nextRequestSequence?: () => bigint,
  reply?: RawReceivedReply
): ZLinkRawReceivedRecord | undefined {
  if (!socket.recv(received, dontWait ? RecvFlags.DontWait : RecvFlags.None)) {
    return undefined;
  }
  try {
    return new NodeRawReceivedRecord(received, nextRequestSequence, reply);
  } catch (error) {
    received.close();
    throw error;
  }
}

/**
 * Continues once on the binding reply. Admission that completed at submit is
 * confirmed in this turn; a backpressured submission keeps the admission
 * wait that bounds it by the request deadline.
 */
function requestReply<T>(
  submission: ReturnType<RequestSubmitOperation['submit']>,
  deadlineMs: number,
  onReply: (parts: readonly Buffer[]) => T,
  onFailed: (error: unknown) => T,
  recordReceived: (() => void) | undefined,
  onAdmitted: (() => void) | undefined
): Promise<T> {
  if (submission.result === SubmitResult.Backpressured) {
    return observeRequestReply(
      bindingRequestReply(submission, deadlineMs, recordReceived, onAdmitted),
      onReply,
      onFailed
    );
  }
  try {
    onAdmitted?.();
  } catch (error) {
    throw translateBindingResultError(error, 'completion');
  }
  return observeRequestReply(submission.reply, onReply, onFailed, recordReceived);
}

/** Consumes the binding reply and notifies its terminal in this continuation. */
function observeRequestReply<T>(
  reply: Promise<readonly Message[]>,
  onReply: (parts: readonly Buffer[]) => T,
  onFailed: (error: unknown) => T,
  recordReceived?: () => void
): Promise<T> {
  return reply.then(
    (replies) => {
      try {
        return onReply(retainAndClose(replies, recordReceived));
      } catch (error) {
        return onFailed(translateBindingResultError(error, 'completion'));
      }
    },
    (error) => onFailed(translateBindingResultError(error, 'completion'))
  );
}

function rethrowCompletion(error: unknown): never {
  throw error;
}

function retainAndClose(
  messages: readonly Message[],
  recordReceived?: () => void
): readonly Buffer[] {
  try {
    recordReceived?.();
    return messages.map((message) => ZLinkBufferMessage.retainPayload(message));
  } finally {
    for (const message of messages) message.close();
  }
}

function bindingRoutingId(routingId: string | Uint8Array): BindingRoutingId {
  return BindingRoutingId.from(routingId);
}

function connectable(socket: Socket): {
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
} {
  if (!('connect' in socket) || !('disconnect' in socket)) {
    throw new TypeError('Raw socket is not connectable.');
  }
  return socket as Socket & {
    connect(endpoint: string): void;
    disconnect(endpoint: string): void;
  };
}

function copyMonitorEvent(event: MonitorEvent): ZLinkRawMonitorRecord {
  return {
    event: event.event,
    value: event.value,
    ...(event.routingId === null ? {} : { routingId: event.routingId.toString() }),
    localAddress: event.localAddr,
    remoteAddress: event.remoteAddr,
    connectionId: event.connectionId,
    transportLane: event.transportLane,
    flags: event.flags
  };
}
