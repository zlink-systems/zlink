import type {
  BaseSocket,
  PollEventFlagValue,
  Socket
} from '@zlink-systems/zlink';
import { zlink } from './node-backend-adapter-support';

/**
 * Owns one binding public Poller and drives it from the socket's libuv
 * readiness callback. PollCompletion is drained only by Poller.wait; the
 * socket callback itself is only the event-loop wakeup.
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

  private readonly onReady = (): void => {
    if (this.disposed) return;
    const ready = this.poller.wait(this.events, 0);
    if (ready === 0 || this.events.hasEvent(0, zlink.PollEventFlag.PollIn)) {
      this.readableHandler();
    }
  };
}
