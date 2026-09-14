import type {
  BaseSocket,
  PollEventFlagValue,
  Socket
} from '@zlink-systems/zlink';
import { zlink } from './node-backend-adapter-support';

/**
 * Owns one binding public Poller. Platform turns drive PollCompletion through
 * poll(); socket receive callbacks do not guarantee completion progress.
 */
export class ZLinkNodeEventLoopPoller {
  private readonly poller = zlink.createPoller();
  private readonly events = zlink.createPollEvents(1);
  private readableHandler: () => void;
  private disposed = false;

  constructor(
    socket: Socket,
    pollCompletion: boolean,
    readableHandler: () => void
  ) {
    this.readableHandler = readableHandler;
    try {
      const pollEvents: PollEventFlagValue[] = [zlink.PollEventFlag.PollIn];
      if (pollCompletion) pollEvents.push(zlink.PollEventFlag.PollCompletion);
      this.poller.add(socket as BaseSocket, pollEvents, 0);
      socket.setReadableHandler(this.onReady);
    } catch (error) {
      this.events.close();
      this.poller.close();
      throw error;
    }
  }

  setReadableHandler(handler: () => void): void {
    this.readableHandler = handler;
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.events.close();
    this.poller.close();
  }

  poll(): number {
    if (this.disposed) return 0;
    const ready = this.poller.wait(this.events, 0);
    return ready === 0 ? 0 : this.events.revents(0);
  }

  private readonly onReady = (): void => {
    if (this.disposed) return;
    const ready = this.poll();
    if (ready === 0 || (ready & zlink.PollEventFlag.PollIn) !== 0) {
      this.readableHandler();
    }
  };
}
