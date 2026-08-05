import {
  Disposable,
  ZlinkStreamErrorCode,
  ZlinkStreamInboundObservation
} from '../Contracts';
import { ZlinkStreamHeaderFlags } from '../Contracts/ZlinkStreamEnums';
import { ZlinkStreamMetadataMap } from '../Contracts/ZlinkStreamMetadata';
import type { ZlinkStreamHeader } from '../Contracts/ZlinkStreamModels';
import { subscription } from './ZlinkStreamSupport';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';

type InboundObserver = (observation: ZlinkStreamInboundObservation, signal?: AbortSignal) => Promise<void> | void;

export class ZlinkStreamInboundObservers {
  private readonly observers = new Set<InboundObserver>();
  private readonly queue: Array<ZlinkStreamInboundObservation | undefined> = [];
  private queueHead = 0;
  private queuedCount = 0;
  private draining = false;
  private dropReportPending = false;

  constructor(
    private readonly maxNotifications: number,
    private readonly maxPayloadPreviewBytes: number,
    private readonly events: ZlinkStreamConnectorEvents
  ) {}

  add(observer: InboundObserver): Disposable {
    this.observers.add(observer);
    return subscription(() => this.observers.delete(observer));
  }

  enqueue(header: ZlinkStreamHeader, payload: Uint8Array, signal?: AbortSignal): void {
    if (this.observers.size === 0) {
      return;
    }

    if (this.queuedCount >= this.maxNotifications) {
      this.reportDropped(signal);
      return;
    }
    this.queue.push(this.createObservation(header, payload));
    this.queuedCount += 1;
    this.scheduleDrain(signal);
  }

  private createObservation(header: ZlinkStreamHeader, payload: Uint8Array): ZlinkStreamInboundObservation {
    const previewLength = Math.min(this.maxPayloadPreviewBytes, payload.length);
    return Object.freeze({
      kind: header.kind,
      name: header.name,
      codec: header.codec,
      requestSeq: header.requestSeq,
      flowId: header.flowId,
      flowOrigin: header.flowOrigin,
      metadata: ZlinkStreamMetadataMap.from(header.metadata.values),
      payloadLength: payload.length,
      isCompressed: (header.flags & ZlinkStreamHeaderFlags.PayloadCompressed) !== 0,
      receivedAt: new Date(),
      payloadPreview: previewLength === 0 ? new Uint8Array() : payload.slice(0, previewLength)
    });
  }

  private scheduleDrain(signal?: AbortSignal): void {
    if (this.draining) {
      return;
    }
    this.draining = true;
    queueMicrotask(() => {
      void this.drain(signal);
    });
  }

  private async drain(signal?: AbortSignal): Promise<void> {
    try {
      while (this.queuedCount > 0) {
        const observation = this.takeObservation();
        if (observation === undefined) continue;
        const observers = [...this.observers];
        for (const observer of observers) {
          if (!this.observers.has(observer)) {
            continue;
          }
          try {
            await observer(observation, signal);
          } catch (cause) {
            await this.reportObserverFailed(cause, signal);
          }
        }
      }
    } finally {
      this.draining = false;
      if (this.queuedCount > 0) {
        this.scheduleDrain(signal);
      }
    }
  }

  private reportDropped(signal?: AbortSignal): void {
    if (this.dropReportPending) {
      return;
    }
    this.dropReportPending = true;
    queueMicrotask(() => {
      void this.events.publishError({
        code: ZlinkStreamErrorCode.ObserverDropped,
        message: 'Inbound observer notification was dropped because the observer queue is full.'
      }, signal).finally(() => {
        this.dropReportPending = false;
      }).catch(() => {});
    });
  }

  private async reportObserverFailed(cause: unknown, signal?: AbortSignal): Promise<void> {
    try {
      await this.events.publishError({
        code: ZlinkStreamErrorCode.ObserverFailed,
        message: 'Inbound observer callback failed.',
        cause
      }, signal);
    } catch {
    }
  }

  private takeObservation(): ZlinkStreamInboundObservation | undefined {
    while (this.queueHead < this.queue.length && this.queue[this.queueHead] === undefined) {
      this.queueHead += 1;
    }
    const observation = this.queue[this.queueHead];
    if (observation === undefined) return undefined;
    this.queue[this.queueHead] = undefined;
    this.queueHead += 1;
    this.queuedCount -= 1;
    if (this.queuedCount === 0) {
      this.queue.length = 0;
      this.queueHead = 0;
    } else if (this.queueHead >= 1024 && this.queueHead * 2 >= this.queue.length) {
      this.queue.splice(0, this.queueHead);
      this.queueHead = 0;
    }
    return observation;
  }
}
