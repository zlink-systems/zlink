import type {
  Disposable,
  ZlinkStreamConnectionStateChanged,
  ZlinkStreamError
} from '../Contracts';
import { subscription } from './ZlinkStreamSupport';

export class ZlinkStreamConnectorEvents {
  private readonly errorHandlers = new Set<(
    error: ZlinkStreamError,
    signal?: AbortSignal
  ) => Promise<void> | void>();
  private readonly disconnectedHandlers = new Set<(signal?: AbortSignal) => Promise<void> | void>();
  private readonly stateHandlers = new Set<(
    change: ZlinkStreamConnectionStateChanged,
    signal?: AbortSignal
  ) => Promise<void> | void>();

  onError(handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void): Disposable {
    this.errorHandlers.add(handler);
    return subscription(() => this.errorHandlers.delete(handler));
  }

  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable {
    this.disconnectedHandlers.add(handler);
    return subscription(() => this.disconnectedHandlers.delete(handler));
  }

  onStateChanged(handler: (
    change: ZlinkStreamConnectionStateChanged,
    signal?: AbortSignal
  ) => Promise<void> | void): Disposable {
    this.stateHandlers.add(handler);
    return subscription(() => this.stateHandlers.delete(handler));
  }

  async publishError(error: ZlinkStreamError, signal?: AbortSignal): Promise<void> {
    await this.publish([...this.errorHandlers].map((handler) => () => handler(error, signal)));
  }

  async publishDisconnected(signal?: AbortSignal): Promise<void> {
    await this.publish([...this.disconnectedHandlers].map((handler) => () => handler(signal)));
  }

  async publishStateChanged(change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal): Promise<void> {
    await this.publish([...this.stateHandlers].map((handler) => () => handler(change, signal)));
  }

  private async publish(handlers: readonly (() => Promise<void> | void)[]): Promise<void> {
    await Promise.allSettled(handlers.map(async (handler) => handler()));
  }
}
