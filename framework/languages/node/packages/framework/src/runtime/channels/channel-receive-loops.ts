import type { RoutingId } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMessageLike as MessageLike } from '../backend/runtime-values';
import type {
  ZLinkBackendRouterSocket,
  ZLinkBackendSpotRouteBridge,
  ZLinkBackendSubscriberSocket,
  ZLinkBackendReadablePoller,
  ZLinkChannelBackendAdapter
} from '../backend/contracts';
import {
  closeMessages
} from './channel-envelope';
import { tryDecodeChannelHeader } from './channel-envelope-inspection';
import type { ZLinkChannelEnvelopeHeader } from './channel-envelope';
import {
  ZLinkReceiveTaskTracker,
  type ZLinkReceiveTaskErrorReporter
} from './channel-receive-task-tracker';
import {
  ApplicationJobQueue,
  type ApplicationJobQueuePermit
} from '../host/application-job-queue';
import { runWithApplicationJobPermit } from '../application-jobs/application-job-queue-scope';

//  The loop awaits between reads and the signal can abort in that gap. Reading
//  through a function keeps the later check honest; an inline read stays
//  narrowed by the loop condition and the comparison is rejected as dead.
function signalAborted(signal?: AbortSignal): boolean {
  return signal?.aborted === true;
}


interface ZLinkMultipartOperation<TNext> {
  message(message: MessageLike): TNext;
}

interface ZLinkMultipartSubmitOperation extends ZLinkMultipartOperation<ZLinkMultipartSubmitOperation> {
  submit(): unknown;
}

type ZLinkMultipartReplyOperation = ZLinkMultipartSubmitOperation;

interface ZLinkChannelRequestDispatchLoop {
  /** Spec 27 §4: with tracing Off, inbound flow fields are neither validated nor carried forward. */
  flowEnabled?(): boolean;
  dispatch(
    received: {
      readonly parts: readonly Message[];
      readonly routingId: unknown;
      readonly spotId?: unknown;
      readonly requestSeq: bigint | null;
      readonly send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
    },
    router: ZLinkBackendRouterSocket & {
      reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
    },
    signal?: AbortSignal,
    decodedHeader?: ZLinkChannelEnvelopeHeader
  ): Promise<boolean | void>;
}

interface ZLinkChannelPublishDispatchLoop {
  dispatch(
    topicMessage: { readonly topic: string; readonly parts: readonly Message[] },
    signal?: AbortSignal,
    decodedHeader?: ZLinkChannelEnvelopeHeader
  ): Promise<void>;
}

interface ZLinkSubscriberInfrastructureResult {
  readonly consumed: boolean;
  readonly decodedHeader?: ZLinkChannelEnvelopeHeader;
}

interface ZLinkRoutePacketDispatchLoop {
  /** Spec 27 §4: with tracing Off, inbound flow fields are neither validated nor carried forward. */
  flowEnabled?(): boolean;
  dispatchInfrastructure?(received: {
    readonly parts: readonly Message[];
    readonly routingId: unknown;
    readonly spotId?: unknown;
    readonly requestSeq: bigint | null;
    readonly send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
  }): boolean;
  dispatch(
    received: {
      readonly parts: readonly Message[];
      readonly routingId: unknown;
      readonly spotId?: unknown;
      readonly requestSeq: bigint | null;
    },
    router: {
      reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
    },
    signal?: AbortSignal,
    decodedHeader?: ZLinkChannelEnvelopeHeader,
    infrastructureChecked?: boolean
  ): Promise<boolean | void>;
}

interface ZLinkChannelInfrastructureHandler {
  (
    received: {
      readonly parts: readonly Message[];
      readonly routingId: unknown;
      readonly requestSeq: bigint | null;
    },
    router: ZLinkBackendRouterSocket
  ): boolean;
}

export class ZLinkReceiveRoundRobinCoordinator {
  private readonly owners: Array<{ readonly token: symbol; ready: boolean }> = [];
  private cursor = 0;
  private active?: symbol;
  private readonly rawReceiveReservations = new ZLinkRawReceiveReservationPool(
    ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT
  );

  register(): symbol {
    const token = Symbol('receive-owner');
    this.owners.push({ token, ready: false });
    return token;
  }

  setReady(token: symbol, ready: boolean): void {
    const owner = this.owners.find(candidate => candidate.token === token);
    if (owner !== undefined) owner.ready = ready;
  }

  tryAcquire(token: symbol): boolean {
    if (this.active !== undefined) return this.active === token;
    const selected = this.nextReadyOwner();
    if (selected?.token !== token) return false;
    this.active = token;
    return true;
  }

  release(token: symbol): void {
    if (this.active !== token) return;
    const index = this.owners.findIndex(owner => owner.token === token);
    this.active = undefined;
    if (this.owners.length > 0) this.cursor = (index + 1) % this.owners.length;
  }

  tryAcquireRawReceiveReservation(): (() => void) | undefined {
    return this.rawReceiveReservations.tryAcquire();
  }

  unregister(token: symbol): void {
    const index = this.owners.findIndex(owner => owner.token === token);
    if (index < 0) return;
    if (this.active === token) this.active = undefined;
    this.owners.splice(index, 1);
    if (this.owners.length === 0) {
      this.cursor = 0;
    } else if (index < this.cursor) {
      this.cursor -= 1;
    } else if (this.cursor >= this.owners.length) {
      this.cursor = 0;
    }
  }

  private nextReadyOwner(): { readonly token: symbol; ready: boolean } | undefined {
    for (let offset = 0; offset < this.owners.length; offset += 1) {
      const owner = this.owners[(this.cursor + offset) % this.owners.length]!;
      if (owner.ready) return owner;
    }
    return undefined;
  }
}

export const ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT = 16;

class ZLinkRawReceiveReservationPool {
  private active = 0;

  constructor(private readonly limit: number) {}

  tryAcquire(): (() => void) | undefined {
    if (this.active >= this.limit) return undefined;
    this.active += 1;
    let released = false;
    return () => {
      if (released) return;
      released = true;
      this.active -= 1;
    };
  }
}

export class ZLinkChannelReceiveLoop {
  //  The loop awaits between the two reads and stop() can land in that gap.
  //  Reading through a method keeps the second check honest; a field read stays
  //  narrowed to false across the await and is treated as dead code.
  private stoppedFlag = false;

  private isStopped(): boolean {
    return this.stoppedFlag;
  }
  private running?: Promise<void>;
  private readonly inFlight: ZLinkReceiveTaskTracker;
  private readonly receiveOwner: symbol;
  private readonly rawReceiveReservations: ZLinkRawReceiveReservationPool;
  private readonly capacityStop = new AbortController();

  constructor(
    private readonly channelName: string,
    private readonly router: ZLinkBackendRouterSocket & {
      reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
    },
    private readonly dispatcher: ZLinkChannelRequestDispatchLoop,
    private readonly spotRouteBridge: ZLinkBackendSpotRouteBridge | undefined,
    private readonly infrastructureHandler: ZLinkChannelInfrastructureHandler | undefined,
    private readonly poller: ZLinkBackendReadablePoller,
    private readonly applicationJobQueue: ApplicationJobQueue,
    reportError?: ZLinkReceiveTaskErrorReporter,
    private readonly roundRobin?: ZLinkReceiveRoundRobinCoordinator
  ) {
    this.inFlight = new ZLinkReceiveTaskTracker(reportError);
    this.receiveOwner = roundRobin?.register() ?? Symbol('channel-receive-owner');
    this.rawReceiveReservations = new ZLinkRawReceiveReservationPool(
      ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT
    );
  }

  async run(signal?: AbortSignal): Promise<void> {
    if (this.running !== undefined) {
      return this.running;
    }
    const running = this.runLoop(signal);
    this.running = running;
    try {
      await running;
    } finally {
      if (this.running === running) {
        this.running = undefined;
      }
    }
  }

  private async runLoop(signal?: AbortSignal): Promise<void> {
    const batch = new ZLinkReceiveBatchBudget();
    const capacitySignal = signal === undefined
      ? this.capacityStop.signal
      : AbortSignal.any([signal, this.capacityStop.signal]);
    while (!this.isStopped() && signal?.aborted !== true) {
      if (!this.poller.wait(0)) {
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const applicationJobPermit = await this.acquirePermit(capacitySignal);
      if (applicationJobPermit === undefined) break;
      if (!this.poller.wait(0)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      this.roundRobin?.setReady(this.receiveOwner, true);
      if (this.roundRobin?.tryAcquire(this.receiveOwner) === false) {
        applicationJobPermit.releaseAfterInternalProcessing();
        await waitReceiveLoopTurn();
        continue;
      }
      const releaseRawReceive = this.roundRobin === undefined
        ? this.rawReceiveReservations.tryAcquire()
        : this.roundRobin.tryAcquireRawReceiveReservation();
      if (releaseRawReceive === undefined) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.release(this.receiveOwner);
        await waitReceiveLoopTurn();
        continue;
      }
      const received = this.router.recv(1);
      if (received == null) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const receivedBytes = messageBytes(received.parts);
      let classification: ReturnType<ZLinkChannelReceiveLoop['classify']>;
      try {
        classification = this.classify(received);
      } catch (error) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        received.close();
        this.inFlight.track(Promise.reject(error));
        continue;
      }
      if (classification.kind === 'consumed') {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        if (classification.closeReceived) received.close();
        if (batch.record(receivedBytes)) {
          this.roundRobin?.release(this.receiveOwner);
          await batch.yieldAndReset();
        }
        continue;
      }
      if (this.isStopped() || signalAborted(signal)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        received.close();
        break;
      }
      applicationJobPermit.markApplicationQueued();
      const task = runWithApplicationJobPermit(applicationJobPermit, () =>
        this.dispatchAndClose(
          received,
          signal,
          classification.decodedHeader,
          true
        ));
      releaseRawReceive();
      this.inFlight.track(task);
      if (batch.record(receivedBytes)) {
        this.roundRobin?.release(this.receiveOwner);
        await batch.yieldAndReset();
      }
    }
  }

  async stop(): Promise<void> {
    this.stoppedFlag = true;
    this.capacityStop.abort();
    try {
      await this.running;
      await this.inFlight.waitForAll();
    } finally {
      this.roundRobin?.unregister(this.receiveOwner);
      this.poller.dispose();
    }
  }

  private async acquirePermit(signal: AbortSignal): Promise<ApplicationJobQueuePermit | undefined> {
    try {
      return await this.applicationJobQueue.acquire(signal);
    } catch (error) {
      if (this.isStopped() || signal.aborted) return undefined;
      throw error;
    }
  }

  private async dispatchAndClose(received: {
    parts: readonly Message[];
    routingId: unknown;
    spotId?: unknown;
    requestSeq: bigint | null;
    send?: () => ZLinkMultipartOperation<ZLinkMultipartSubmitOperation>;
    close(): void;
  }, signal?: AbortSignal, decodedHeader?: ZLinkChannelEnvelopeHeader, infrastructureChecked = false, releaseRawReceive?: () => void): Promise<void> {
    let closeReceived = true;
    try {
      if (!infrastructureChecked) {
        const classification = this.classify(received);
        if (classification.kind === 'consumed') {
          closeReceived = classification.closeReceived;
          return;
        }
        decodedHeader = classification.decodedHeader;
      }
        const consumed = await this.dispatcher.dispatch(
          received,
          this.router,
          signal,
          decodedHeader
        );
        if (consumed === true) {
          closeReceived = false;
        }
    } finally {
      releaseRawReceive?.();
      if (closeReceived) {
        received.close();
      }
    }
  }

  private classify(received: {
    parts: readonly Message[];
    routingId: unknown;
    requestSeq: bigint | null;
  }):
    | { readonly kind: 'consumed'; readonly closeReceived: boolean }
    | { readonly kind: 'application'; readonly decodedHeader?: ZLinkChannelEnvelopeHeader } {
    if (this.infrastructureHandler?.(received, this.router) === true) {
      return { kind: 'consumed', closeReceived: true };
    }
    const decodedHeader = tryDecodeChannelHeader(received.parts, this.dispatcher.flowEnabled?.() ?? true);
    if (
      decodedHeader === undefined
      && this.spotRouteBridge?.handleRouterReceived(
        this.channelName,
        received.routingId as RoutingId,
        received.requestSeq ?? 0n,
        received.parts
      ) === true
    ) {
      return { kind: 'consumed', closeReceived: false };
    }
    return { kind: 'application', decodedHeader };
  }
}

export class ZLinkSubscriberReceiveLoop {
  //  The loop awaits between the two reads and stop() can land in that gap.
  //  Reading through a method keeps the second check honest; a field read stays
  //  narrowed to false across the await and is treated as dead code.
  private stoppedFlag = false;

  private isStopped(): boolean {
    return this.stoppedFlag;
  }
  private running?: Promise<void>;
  private readonly inFlight: ZLinkReceiveTaskTracker;
  private readonly receiveOwner: symbol;
  private readonly rawReceiveReservations: ZLinkRawReceiveReservationPool;
  private readonly capacityStop = new AbortController();

  constructor(
    private readonly adapter: ZLinkChannelBackendAdapter,
    private readonly subscriber: ZLinkBackendSubscriberSocket,
    private readonly dispatcher: ZLinkChannelPublishDispatchLoop,
    private readonly applicationJobQueue: ApplicationJobQueue,
    private readonly infrastructureHandler?: (
      topicMessage: ReturnType<ZLinkChannelBackendAdapter['createTopicMessage']>
    ) => boolean | ZLinkSubscriberInfrastructureResult,
    private readonly roundRobin?: ZLinkReceiveRoundRobinCoordinator,
    reportError?: ZLinkReceiveTaskErrorReporter
  ) {
    this.inFlight = new ZLinkReceiveTaskTracker(reportError);
    this.poller = adapter.createReadablePoller(subscriber);
    this.receiveOwner = roundRobin?.register() ?? Symbol('subscriber-receive-owner');
    this.rawReceiveReservations = new ZLinkRawReceiveReservationPool(
      ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT
    );
  }

  private readonly poller: ReturnType<ZLinkChannelBackendAdapter['createReadablePoller']>;

  async run(signal?: AbortSignal): Promise<void> {
    if (this.running !== undefined) {
      return this.running;
    }
    const running = this.runLoop(signal);
    this.running = running;
    try {
      await running;
    } finally {
      if (this.running === running) {
        this.running = undefined;
      }
    }
  }

  private async runLoop(signal?: AbortSignal): Promise<void> {
    const batch = new ZLinkReceiveBatchBudget();
    const capacitySignal = signal === undefined
      ? this.capacityStop.signal
      : AbortSignal.any([signal, this.capacityStop.signal]);
    while (!this.isStopped() && signal?.aborted !== true) {
      if (!this.poller.wait(0)) {
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const applicationJobPermit = await this.acquirePermit(capacitySignal);
      if (applicationJobPermit === undefined) break;
      if (!this.poller.wait(0)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      this.roundRobin?.setReady(this.receiveOwner, true);
      if (this.roundRobin?.tryAcquire(this.receiveOwner) === false) {
        applicationJobPermit.releaseAfterInternalProcessing();
        await waitReceiveLoopTurn();
        continue;
      }
      const releaseRawReceive = this.roundRobin === undefined
        ? this.rawReceiveReservations.tryAcquire()
        : this.roundRobin.tryAcquireRawReceiveReservation();
      if (releaseRawReceive === undefined) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.release(this.receiveOwner);
        await waitReceiveLoopTurn();
        continue;
      }
      const topicMessage = this.adapter.createTopicMessage();
      if (!this.subscriber.subscribe(topicMessage)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        topicMessage.close();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const receivedBytes = messageBytes(topicMessage.parts as readonly Message[]);
      let classification: ReturnType<ZLinkSubscriberReceiveLoop['classify']>;
      try {
        classification = this.classify(topicMessage);
      } catch (error) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        closeMessages(topicMessage.parts as readonly Message[]);
        this.inFlight.track(Promise.reject(error));
        continue;
      }
      if (classification.kind === 'consumed') {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        closeMessages(topicMessage.parts as readonly Message[]);
        if (batch.record(receivedBytes)) {
          this.roundRobin?.release(this.receiveOwner);
          await batch.yieldAndReset();
        }
        continue;
      }
      if (this.isStopped() || signalAborted(signal)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        closeMessages(topicMessage.parts as readonly Message[]);
        break;
      }
      applicationJobPermit.markApplicationQueued();
      const task = runWithApplicationJobPermit(applicationJobPermit, () =>
        this.dispatchAndClose(
          topicMessage,
          signal,
          classification.decodedHeader,
          true
        ));
      releaseRawReceive();
      this.inFlight.track(task);
      if (batch.record(receivedBytes)) {
        this.roundRobin?.release(this.receiveOwner);
        await batch.yieldAndReset();
      }
    }
  }

  async stop(): Promise<void> {
    this.stoppedFlag = true;
    this.capacityStop.abort();
    try {
      await this.running;
      await this.inFlight.waitForAll();
    } finally {
      this.roundRobin?.unregister(this.receiveOwner);
      this.poller.dispose();
    }
  }

  private async acquirePermit(signal: AbortSignal): Promise<ApplicationJobQueuePermit | undefined> {
    try {
      return await this.applicationJobQueue.acquire(signal);
    } catch (error) {
      if (this.isStopped() || signal.aborted) return undefined;
      throw error;
    }
  }

  private async dispatchAndClose(
    topicMessage: ReturnType<ZLinkChannelBackendAdapter['createTopicMessage']>,
    signal?: AbortSignal,
    decodedHeader?: ZLinkChannelEnvelopeHeader,
    infrastructureChecked = false,
    releaseRawReceive?: () => void
  ): Promise<void> {
    try {
      if (!infrastructureChecked) {
        const classification = this.classify(topicMessage);
        if (classification.kind === 'consumed') return;
        decodedHeader = classification.decodedHeader;
      }
        await this.dispatcher.dispatch(topicMessage, signal, decodedHeader);
    } finally {
      releaseRawReceive?.();
      closeMessages(topicMessage.parts as readonly Message[]);
    }
  }

  private classify(
    topicMessage: ReturnType<ZLinkChannelBackendAdapter['createTopicMessage']>
  ):
    | { readonly kind: 'consumed' }
    | { readonly kind: 'application'; readonly decodedHeader?: ZLinkChannelEnvelopeHeader } {
    const infrastructureResult = this.infrastructureHandler?.(topicMessage);
    const decodedHeader = typeof infrastructureResult === 'object'
      ? infrastructureResult.decodedHeader
      : undefined;
    if (
      infrastructureResult === true
      || (typeof infrastructureResult === 'object' && infrastructureResult.consumed)
    ) {
      return { kind: 'consumed' };
    }
    return { kind: 'application', decodedHeader };
  }
}

export class ZLinkRouteReceiveLoop {
  //  The loop awaits between the two reads and stop() can land in that gap.
  //  Reading through a method keeps the second check honest; a field read stays
  //  narrowed to false across the await and is treated as dead code.
  private stoppedFlag = false;

  private isStopped(): boolean {
    return this.stoppedFlag;
  }
  private running?: Promise<void>;
  private readonly inFlight: ZLinkReceiveTaskTracker;
  private readonly receiveOwner: symbol;
  private readonly rawReceiveReservations: ZLinkRawReceiveReservationPool;
  private readonly capacityStop = new AbortController();

  constructor(
    private readonly router: ZLinkBackendRouterSocket & {
      reply(routingId: unknown, requestSeq: bigint): ZLinkMultipartReplyOperation;
    },
    private readonly dispatcher: ZLinkRoutePacketDispatchLoop,
    private readonly poller: ZLinkBackendReadablePoller,
    private readonly applicationJobQueue: ApplicationJobQueue,
    private readonly roundRobin?: ZLinkReceiveRoundRobinCoordinator,
    reportError?: ZLinkReceiveTaskErrorReporter
  ) {
    this.inFlight = new ZLinkReceiveTaskTracker(reportError);
    this.receiveOwner = roundRobin?.register() ?? Symbol('route-receive-owner');
    this.rawReceiveReservations = new ZLinkRawReceiveReservationPool(
      ZLINK_CHANNEL_RAW_RECEIVE_RESERVATION_LIMIT
    );
  }

  async run(signal?: AbortSignal): Promise<void> {
    if (this.running !== undefined) {
      return this.running;
    }
    const running = this.runLoop(signal);
    this.running = running;
    try {
      await running;
    } finally {
      if (this.running === running) {
        this.running = undefined;
      }
    }
  }

  private async runLoop(signal?: AbortSignal): Promise<void> {
    const batch = new ZLinkReceiveBatchBudget();
    const capacitySignal = signal === undefined
      ? this.capacityStop.signal
      : AbortSignal.any([signal, this.capacityStop.signal]);
    while (!this.isStopped() && signal?.aborted !== true) {
      if (!this.poller.wait(0)) {
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const applicationJobPermit = await this.acquirePermit(capacitySignal);
      if (applicationJobPermit === undefined) break;
      if (!this.poller.wait(0)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      this.roundRobin?.setReady(this.receiveOwner, true);
      if (this.roundRobin?.tryAcquire(this.receiveOwner) === false) {
        applicationJobPermit.releaseAfterInternalProcessing();
        await waitReceiveLoopTurn();
        continue;
      }
      const releaseRawReceive = this.roundRobin === undefined
        ? this.rawReceiveReservations.tryAcquire()
        : this.roundRobin.tryAcquireRawReceiveReservation();
      if (releaseRawReceive === undefined) {
        applicationJobPermit.releaseAfterInternalProcessing();
        this.roundRobin?.release(this.receiveOwner);
        await waitReceiveLoopTurn();
        continue;
      }
      const received = this.router.recv(1);
      if (received == null) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        this.roundRobin?.setReady(this.receiveOwner, false);
        this.roundRobin?.release(this.receiveOwner);
        batch.reset();
        await waitReceiveLoopIdle();
        continue;
      }
      const receivedBytes = messageBytes(received.parts);
      let infrastructureConsumed: boolean;
      try {
        infrastructureConsumed = this.dispatcher.dispatchInfrastructure?.(received) === true;
      } catch (error) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        received.close();
        this.inFlight.track(Promise.reject(error));
        continue;
      }
      if (infrastructureConsumed) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        if (batch.record(receivedBytes)) {
          this.roundRobin?.release(this.receiveOwner);
          await batch.yieldAndReset();
        }
        continue;
      }
      if (this.isStopped() || signalAborted(signal)) {
        applicationJobPermit.releaseAfterInternalProcessing();
        releaseRawReceive();
        received.close();
        break;
      }
      applicationJobPermit.markApplicationQueued();
      const task = runWithApplicationJobPermit(applicationJobPermit, () =>
        this.dispatchAndClose(received, signal, true));
      releaseRawReceive();
      this.inFlight.track(task);
      if (batch.record(receivedBytes)) {
        this.roundRobin?.release(this.receiveOwner);
        await batch.yieldAndReset();
      }
    }
  }

  async stop(): Promise<void> {
    this.stoppedFlag = true;
    this.capacityStop.abort();
    try {
      await this.running;
      await this.inFlight.waitForAll();
    } finally {
      this.roundRobin?.unregister(this.receiveOwner);
      this.poller.dispose();
    }
  }

  private async acquirePermit(signal: AbortSignal): Promise<ApplicationJobQueuePermit | undefined> {
    try {
      return await this.applicationJobQueue.acquire(signal);
    } catch (error) {
      if (this.isStopped() || signal.aborted) return undefined;
      throw error;
    }
  }

  private async dispatchAndClose(received: {
    parts: readonly Message[];
    routingId: unknown;
    spotId?: unknown;
    requestSeq: bigint | null;
    close(): void;
  }, signal?: AbortSignal, infrastructureChecked = false, releaseRawReceive?: () => void): Promise<void> {
    let closeReceived = true;
      try {
        const consumed = await this.dispatcher.dispatch(
        received,
        this.router,
        signal,
        tryDecodeChannelHeader(received.parts, this.dispatcher.flowEnabled?.() ?? true),
        infrastructureChecked
      );
      if (consumed === true) {
        closeReceived = false;
      }
      } finally {
        releaseRawReceive?.();
        if (closeReceived) {
        received.close();
      }
    }
  }
}

const RECEIVE_BATCH_MESSAGE_LIMIT = 64;
const RECEIVE_BATCH_BYTE_LIMIT = 4n * 1024n * 1024n;
const RECEIVE_BATCH_TIME_LIMIT_MS = 2;

class ZLinkReceiveBatchBudget {
  private messages = 0;
  private bytes = 0n;
  private startedAt = performance.now();

  record(bytes: bigint): boolean {
    this.messages += 1;
    this.bytes += bytes;
    return this.messages >= RECEIVE_BATCH_MESSAGE_LIMIT
      || this.bytes >= RECEIVE_BATCH_BYTE_LIMIT
      || performance.now() - this.startedAt >= RECEIVE_BATCH_TIME_LIMIT_MS;
  }

  async yieldAndReset(): Promise<void> {
    await new Promise<void>(resolve => setImmediate(resolve));
    this.reset();
  }

  reset(): void {
    this.messages = 0;
    this.bytes = 0n;
    this.startedAt = performance.now();
  }
}

class ZLinkSharedIdleWaiter {
  private pending?: Promise<void>;
  private resolvePending?: () => void;
  private readonly wake = (): void => {
    const resolve = this.resolvePending;
    this.pending = undefined;
    this.resolvePending = undefined;
    resolve?.();
  };

  constructor(private readonly delayMs: number) {}

  wait(): Promise<void> {
    if (this.pending !== undefined) return this.pending;
    this.pending = new Promise<void>((resolve) => {
      this.resolvePending = resolve;
    });
    setTimeout(this.wake, this.delayMs);
    return this.pending;
  }
}

const CHANNEL_RECEIVE_IDLE_POLL_INTERVAL_MS = 5;
const receiveLoopIdleWaiter = new ZLinkSharedIdleWaiter(
  CHANNEL_RECEIVE_IDLE_POLL_INTERVAL_MS
);

function waitReceiveLoopIdle(): Promise<void> {
  return receiveLoopIdleWaiter.wait();
}

function waitReceiveLoopTurn(): Promise<void> {
  return new Promise<void>(resolve => setImmediate(resolve));
}


function messageBytes(parts: readonly Message[]): bigint {
  return parts.reduce((sum, part) => sum + messagePartBytes(part), 0n);
}

function messagePartBytes(part: Message | undefined): bigint {
  if (part === undefined) {
    return 0n;
  }
  const size = (part as Message & { size?: () => number }).size;
  if (typeof size === 'function') {
    return BigInt(size.call(part));
  }
  return BigInt(part.data().byteLength);
}
