import {
  Message,
  Received,
  RoutingId as BindingRoutingId,
  SendFlags,
  createContext,
  createDealerSocket,
  createPollEvents,
  createPoller,
  createRouterSocket,
  PollEventFlag,
  type Context,
  type DealerSocket,
  type MonitorEvent,
  type MonitorSocket,
  type ReplyOperation,
  type ReplySubmitOperation,
  type RequestOperation,
  type RequestSubmitOperation,
  type RouterSocket,
  type SendOperation,
  type SendSubmitOperation,
  type Socket
} from '@zlink-systems/zlink';
import { isPollerInterruptedError } from './node-backend-adapter-support';
import { isEndpointCloseIgnorableError } from './node-socket-backend-adapter';
import { ZLinkAsyncSubmitter } from '../../messaging';

export interface ZLinkRawReceivedRecord {
  readonly sourceRid: string;
  readonly sourceRoute: Uint8Array;
  readonly requestSeq?: bigint;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
  readonly reply?: (parts: readonly Uint8Array[]) => void;
  readonly parts: readonly Buffer[];
}

export interface ZLinkRawMonitorRecord {
  readonly event: number;
  readonly value: number;
  readonly routingId?: string;
  readonly localAddress: string;
  readonly remoteAddress: string;
  readonly connectionId?: bigint;
  readonly transportPairId?: bigint;
  readonly transportPairGeneration?: bigint;
  readonly transportLane?: number;
  readonly flags?: number;
}

export interface ZLinkRawSocketPort {
  bind(endpoint: string): void;
  unbind(endpoint: string): void;
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
  monitor(handler: (event: ZLinkRawMonitorRecord) => void): ZLinkRawMonitorPort;
  close(): void;
}

export interface ZLinkRawMonitorPort {
  statusReady(): boolean;
  close(): void;
}

export interface ZLinkRawRouterPort extends ZLinkRawSocketPort {
  /** Disconnects the route identified by the peer RoutingId, including an inbound route. */
  disconnectRid?(routingId: string): void;
  /** Disconnects only the physical transport pair identified by monitor identity. */
  disconnectTransportPair?(transportPairId: bigint, transportPairGeneration: bigint): void;
  localEndpoint(): string;
  setRoutingId(routingId: string): void;
  connectToRoutingId(routingId: string, endpoint: string): void;
  send(targetRid: string, parts: readonly Uint8Array[], dontWait?: boolean): boolean;
  sendTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    dontWait?: boolean
  ): boolean;
  request(
    targetRid: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]>;
  requestTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]>;
  receive(dontWait?: boolean): ZLinkRawReceivedRecord | undefined;
  reply(
    targetRid: string | Uint8Array,
    requestSeq: bigint,
    parts: readonly Uint8Array[]
  ): void;
  /**
   * Submits a bounded Framework control record on the existing Completion
   * connection. Application payload must continue to use send().
   */
  trySendCompletionControl?(
    targetRid: string,
    parts: readonly Uint8Array[]
  ): boolean;
  trySendCompletionControlTransportPair?(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[]
  ): boolean;
  /**
   * Receives Framework control records without consuming Application
   * connection messages. The handler owns copied payload bytes.
   */
  setCompletionControlHandler?(
    handler: (sourceRid: string, parts: readonly Buffer[]) => void
  ): void;
  /** Progresses only Completion connection work and never consumes Application messages. */
  progressCompletion?(): number;
}

export interface ZLinkRawDealerPort extends ZLinkRawSocketPort {
  setRoutingId(routingId: string): void;
  send(parts: readonly Uint8Array[], dontWait?: boolean): boolean;
  request(parts: readonly Uint8Array[], timeoutMs: number): Promise<readonly Buffer[]>;
  receive(dontWait?: boolean): ZLinkRawReceivedRecord | undefined;
}

export interface ZLinkRawHostPort {
  createRouter(): ZLinkRawRouterPort;
  createDealer(): ZLinkRawDealerPort;
  shutdown(): void;
  close(): void;
}

export interface ZLinkRawBindingPort {
  createHost(): ZLinkRawHostPort;
}

export class ZLinkNodeRawBindingPort implements ZLinkRawBindingPort {
  constructor(private readonly sharedContext?: Context) {}

  createHost(): ZLinkRawHostPort {
    return new NodeRawHostPort(
      this.sharedContext ?? createContext(),
      this.sharedContext === undefined
    );
  }
}

class NodeRawHostPort implements ZLinkRawHostPort {
  private readonly resources: Array<{ close(): void }> = [];
  private state: 'open' | 'stopping' | 'closed' = 'open';

  constructor(
    private readonly context: Context,
    private readonly ownsContext: boolean
  ) {}

  createRouter(): ZLinkRawRouterPort {
    this.requireOpen();
    return this.own(new NodeRawRouterPort(createRouterSocket(this.context)));
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
    for (const resource of this.resources.splice(0).reverse()) {
      try {
        resource.close();
      } catch (error) {
        failures.push(error);
      }
    }
    if (this.ownsContext) {
      try {
        this.context.close();
      } catch (error) {
        failures.push(error);
      }
    }
    this.state = 'closed';
    if (failures.length > 0) {
      throw new AggregateError(failures, 'Raw binding host cleanup failed.');
    }
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
  private readonly endpoints = new Set<string>();
  private readonly monitors = new Set<NodeRawMonitorPort>();
  private readonly readablePoller = createPoller();
  private readonly readableEvents = createPollEvents(1);
  // Raw receive copies the parts into the Framework-owned mailbox before the
  // next receive begins, so the binding envelope can be reused safely.
  private readonly received = new Received();
  private closed = false;

  protected constructor(protected readonly socket: TSocket) {
    this.readablePoller.add(socket as never, [PollEventFlag.PollIn], 0);
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

  monitor(handler: (event: ZLinkRawMonitorRecord) => void): ZLinkRawMonitorPort {
    this.requireOpen();
    const nativeMonitor = this.socket.monitorOpen();
    const port = new NodeRawMonitorPort(nativeMonitor, () => this.monitors.delete(port));
    this.monitors.add(port);
    nativeMonitor.onEvent(event => handler(copyMonitorEvent(event)));
    return port;
  }

  close(): void {
    if (this.closed) return;
    const failures: unknown[] = [];
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
    this.endpoints.clear();
    try {
      this.readablePoller.remove(this.socket as never);
    } catch (error) {
      failures.push(error);
    }
    try {
      this.readableEvents.close();
    } catch (error) {
      failures.push(error);
    }
    try {
      this.readablePoller.close();
    } catch (error) {
      failures.push(error);
    }
    try {
      this.socket.close();
    } catch (error) {
      failures.push(error);
    }
    try {
      this.received.close();
    } catch (error) {
      failures.push(error);
    }
    this.closed = true;
    if (failures.length > 0) {
      console.error('Raw socket cleanup causes:', failures);
      throw new AggregateError(failures, 'Raw socket cleanup failed.');
    }
  }

  protected requireOpen(): void {
    if (this.closed) throw new Error('Raw socket is closed.');
  }

  protected receiveRecord(
    dontWait: boolean,
    reply?: RawReceivedReply
  ): ZLinkRawReceivedRecord | undefined {
    const timeoutMs = dontWait ? 0 : -1;
    try {
      if (
        this.readablePoller.wait(this.readableEvents, timeoutMs) <= 0
        || !this.readableEvents.hasEvent(0, PollEventFlag.PollIn)
      ) {
        return undefined;
      }
    } catch (error) {
      if (isPollerInterruptedError(error)) return undefined;
      throw error;
    }
    return receiveRecord(this.socket as never, true, reply, this.received);
  }
}

class NodeRawRouterPort extends NodeRawSocketPort<RouterSocket> implements ZLinkRawRouterPort {
  private readonly completionPoller = createPoller();
  private readonly completionEvents = createPollEvents(1);
  private readonly replySubmitter: ZLinkAsyncSubmitter;
  private completionTimer?: ReturnType<typeof setTimeout>;
  private completionClosed = false;

  disconnectRid(routingId: string): void {
    this.requireOpen();
    (this.socket as RouterSocket & {
      disconnectRid(value: BindingRoutingId): void;
    }).disconnectRid(bindingRoutingId(routingId));
  }

  disconnectTransportPair(transportPairId: bigint, transportPairGeneration: bigint): void {
    this.requireOpen();
    (this.socket as RouterSocket & {
      disconnectTransportPair(id: bigint, generation: bigint): void;
    }).disconnectTransportPair(transportPairId, transportPairGeneration);
  }

  constructor(socket: RouterSocket) {
    super(socket);
    socket.options.handover = true;
    socket.options.mandatory = true;
    socket.options.probe = true;
    this.replySubmitter = new ZLinkAsyncSubmitter(
      handler => socket.setSendReadyHandler(handler),
      {
        timeoutMs: socket.options.requestTimeout > 0
          ? socket.options.requestTimeout
          : 30_000,
        capacity: 4096
      }
    );
    this.completionPoller.add(socket, [PollEventFlag.PollCompletion], 1);
  }

  localEndpoint(): string {
    this.requireOpen();
    return this.socket.options.lastEndpoint;
  }

  setRoutingId(routingId: string): void {
    this.requireOpen();
    this.socket.setRoutingId(bindingRoutingId(routingId));
  }

  connectToRoutingId(routingId: string, endpoint: string): void {
    this.requireOpen();
    this.socket.options.setConnectRoutingId(bindingRoutingId(routingId));
    this.connect(endpoint);
  }

  send(targetRid: string, parts: readonly Uint8Array[], dontWait = false): boolean {
    this.requireOpen();
    return appendSendParts(this.socket.send(bindingRoutingId(targetRid)), parts)
      .flags(dontWait ? SendFlags.DontWait : SendFlags.None)
      .submit();
  }

  sendTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    dontWait = false
  ): boolean {
    this.requireOpen();
    this.socket.sendTransportPair(
      bindingRoutingId(targetRid),
      transportPairId,
      transportPairGeneration,
      parts,
      dontWait ? SendFlags.DontWait : SendFlags.None
    );
    return true;
  }

  async request(
    targetRid: string,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]> {
    this.requireOpen();
    const replies = await appendRequestParts(this.socket.request(bindingRoutingId(targetRid)), parts)
      .timeout(timeoutMs)
      .submit();
    return copyAndClose(replies);
  }

  async requestTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[],
    timeoutMs: number
  ): Promise<readonly Buffer[]> {
    this.requireOpen();
    const replies = await appendRequestParts(
      this.socket.requestTransportPair(
        bindingRoutingId(targetRid),
        transportPairId,
        transportPairGeneration
      ),
      parts
    ).timeout(timeoutMs).submit();
    return copyAndClose(replies);
  }

  receive(dontWait = false): ZLinkRawReceivedRecord | undefined {
    this.requireOpen();
    return this.receiveRecord(dontWait, (targetRid, requestSeq, parts) => {
      this.reply(targetRid, requestSeq, parts);
    });
  }

  reply(targetRid: string | Uint8Array, requestSeq: bigint, parts: readonly Uint8Array[]): void {
    this.requireOpen();
    if (this.tryReply(targetRid, requestSeq, parts)) return;
    // A request reply is already accepted work. Keep it on the bounded
    // send-ready queue when the peer's completion lane is temporarily full;
    // application sends must not be given this treatment at their call site.
    this.replySubmitter.submitCommandOneWay(
      () => this.tryReply(targetRid, requestSeq, parts)
    );
  }

  trySendCompletionControl(
    targetRid: string,
    parts: readonly Uint8Array[]
  ): boolean {
    this.requireOpen();
    // The public binding accepts MessageLike values and performs the native
    // handoff itself. Do not create a temporary Message for every control
    // part; the input is explicitly non-consuming.
    return this.socket.trySendCompletionControl(bindingRoutingId(targetRid), parts);
  }

  trySendCompletionControlTransportPair(
    targetRid: string,
    transportPairId: bigint,
    transportPairGeneration: bigint,
    parts: readonly Uint8Array[]
  ): boolean {
    this.requireOpen();
    return this.socket.trySendCompletionControlTransportPair(
      bindingRoutingId(targetRid),
      transportPairId,
      transportPairGeneration,
      parts
    );
  }

  setCompletionControlHandler(
    handler: (sourceRid: string, parts: readonly Buffer[]) => void
  ): void {
    this.requireOpen();
    this.socket.setCompletionControlHandler((sourceRoutingId, messages) => {
      try {
        handler(
          sourceRoutingId.toString(),
          messages.map(message => message.toBytes())
        );
      } finally {
        for (const message of messages) message.close();
      }
    });
    this.scheduleCompletionProgress();
  }

  progressCompletion(): number {
    this.requireOpen();
    try {
      return this.completionPoller.wait(this.completionEvents, 0);
    } catch (error) {
      if (isPollerInterruptedError(error)) return 0;
      throw error;
    }
  }

  override close(): void {
    this.completionClosed = true;
    this.replySubmitter.dispose();
    if (this.completionTimer !== undefined) {
      clearTimeout(this.completionTimer);
      this.completionTimer = undefined;
    }
    try {
      this.completionPoller.remove(this.socket);
    } finally {
      this.completionEvents.close();
      this.completionPoller.close();
      super.close();
    }
  }

  private scheduleCompletionProgress(): void {
    if (this.completionClosed || this.completionTimer !== undefined) return;
    this.completionTimer = setTimeout(() => {
      this.completionTimer = undefined;
      if (this.completionClosed) return;
      try {
        this.progressCompletion();
      } finally {
        this.scheduleCompletionProgress();
      }
    }, 5);
    this.completionTimer.unref();
  }

  private tryReply(
    targetRid: string | Uint8Array,
    requestSeq: bigint,
    parts: readonly Uint8Array[]
  ): boolean {
    try {
      appendReplyParts(this.socket.reply(bindingRoutingId(targetRid), requestSeq), parts).submit();
      return true;
    } catch (error) {
      if (isReplyBackpressured(error)) return false;
      throw error;
    }
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

  send(parts: readonly Uint8Array[], dontWait = false): boolean {
    this.requireOpen();
    return appendSendParts(this.socket.send(), parts)
      .flags(dontWait ? SendFlags.DontWait : SendFlags.None)
      .submit();
  }

  async request(parts: readonly Uint8Array[], timeoutMs: number): Promise<readonly Buffer[]> {
    this.requireOpen();
    const replies = await appendRequestParts(this.socket.request(), parts).timeout(timeoutMs).submit();
    return copyAndClose(replies);
  }

  receive(dontWait = false): ZLinkRawReceivedRecord | undefined {
    this.requireOpen();
    return this.receiveRecord(dontWait);
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

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.release();
    this.monitor.close();
  }
}

function appendSendParts(
  operation: SendOperation,
  parts: readonly Uint8Array[]
): SendSubmitOperation {
  requireParts(parts);
  let next = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index += 1) {
    next = next.message(parts[index]);
  }
  return next;
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

function isReplyBackpressured(error: unknown): boolean {
  if (typeof error !== 'object' || error === null) return false;
  const nativeErrno = (error as { readonly nativeErrno?: unknown }).nativeErrno;
  if (nativeErrno === 11) return true;
  return /resource temporarily unavailable|eagain/i.test(
    String((error as { readonly message?: unknown }).message ?? '')
  );
}

function requireParts(parts: readonly Uint8Array[]): [Uint8Array, ...Uint8Array[]] {
  if (parts.length === 0) throw new TypeError('Raw multipart operation requires at least one part.');
  return parts as [Uint8Array, ...Uint8Array[]];
}

type RawReceivedReply = (
  targetRid: Uint8Array,
  requestSeq: bigint,
  parts: readonly Uint8Array[]
) => void;

function receiveRecord(
  socket: Pick<RouterSocket | DealerSocket, 'recv'>,
  dontWait: boolean,
  reply: RawReceivedReply | undefined,
  received: Received
): ZLinkRawReceivedRecord | undefined {
  if (!socket.recv(received, dontWait ? 1 : 0)) {
    received.close();
    return undefined;
  }
  try {
    const sourceRid = received.routingId?.toString() ?? '';
    const sourceRoute = received.routingId?.toBytes() ?? Buffer.alloc(0);
    const requestSeq = received.requestSeq;
    const transportPairId = received.transportPairId;
    const transportPairGeneration = received.transportPairGeneration;
    const receivedReply = requestSeq === null
      ? undefined
      : reply === undefined
        ? received.reply()
        : (parts: readonly Uint8Array[]) => {
            reply(sourceRoute, requestSeq, parts);
          };
    return {
      sourceRid,
      sourceRoute,
      ...(requestSeq === null ? {} : { requestSeq }),
      ...(transportPairId === undefined ? {} : { transportPairId }),
      ...(transportPairGeneration === undefined ? {} : { transportPairGeneration }),
      ...(receivedReply === undefined
        ? {}
        : {
            reply: (parts: readonly Uint8Array[]) => {
              if (typeof receivedReply === 'function') {
                receivedReply(parts);
                return;
              }
              appendReplyParts(receivedReply, parts).submit();
            }
          }),
      parts: received.parts.map(part => part.toBytes())
    };
  } finally {
    received.close();
  }
}

function copyAndClose(messages: readonly Message[]): readonly Buffer[] {
  try {
    return messages.map(message => message.toBytes());
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
    transportPairId: event.transportPairId,
    transportPairGeneration: event.transportPairGeneration,
    transportLane: event.transportLane,
    flags: event.flags
  };
}
