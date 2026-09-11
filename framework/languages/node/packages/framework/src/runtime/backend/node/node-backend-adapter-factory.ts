import type {
  Context,
  CoreHwmBudgetSnapshot,
  TopicMessage
} from '@zlink-systems/zlink';
import type { ZLinkCoreHwmOptions } from '../../../contracts/Configuration';
import type {
  ZLinkBackendAdapterFactory,
  ZLinkBackendContext,
  ZLinkBackendDealerSocket,
  ZLinkBackendPublisherSocket,
  ZLinkBackendReadablePoller,
  ZLinkBackendReceived,
  ZLinkBackendRouterSocket,
  ZLinkBackendSocket,
  ZLinkBackendSocketMonitor,
  ZLinkBackendStreamPacket,
  ZLinkBackendStreamSocket,
  ZLinkBackendSubscriberSocket,
  ZLinkChannelBackendAdapter,
  ZLinkMonitoringBackendAdapter,
  ZLinkStreamBackendAdapter
} from '../contracts';
import {
  closeWithBusyRetry,
  isContextTerminatedError,
  zlink,
  type ZLinkBindingModule
} from './node-backend-adapter-support';
import { wrapMonitorSocket } from './node-monitor-backend-adapter';
import { wrapSocket } from './node-socket-backend-adapter';
import { ZLinkNodeMeshBackendAdapter } from './node-mesh-backend-adapter';

export { isDisconnectRouteNotFoundError } from './node-socket-backend-adapter';

export class ZLinkNodeBackendAdapterFactory implements ZLinkBackendAdapterFactory {
  createReceived(): ZLinkBackendReceived {
    return new zlink.Received();
  }

  createTopicMessage(): TopicMessage {
    return new zlink.TopicMessage();
  }

  createChannelAdapter(): ZLinkChannelBackendAdapter {
    return new ZLinkNodeChannelBackendAdapter();
  }

  createMeshAdapter(): ZLinkNodeMeshBackendAdapter {
    return new ZLinkNodeMeshBackendAdapter();
  }

  createStreamAdapter(): ZLinkStreamBackendAdapter {
    return new ZLinkNodeStreamBackendAdapter();
  }

  createMonitoringAdapter(): ZLinkMonitoringBackendAdapter {
    return new ZLinkNodeMonitoringBackendAdapter();
  }
}

class ZLinkNodeChannelBackendAdapter implements ZLinkChannelBackendAdapter {
  createContext(): ZLinkBackendContext {
    return new ZLinkNodeBackendContext(zlink.createContext());
  }

  createTopicMessage(): TopicMessage {
    return new zlink.TopicMessage();
  }

  createDealerSocket(context: ZLinkBackendContext): ZLinkBackendDealerSocket {
    return wrapSocket(zlink.createDealerSocket(asNodeContext(context))) as unknown as ZLinkBackendDealerSocket;
  }

  createRouterSocket(context: ZLinkBackendContext): ZLinkBackendRouterSocket {
    return wrapSocket(zlink.createRouterSocket(asNodeContext(context))) as unknown as ZLinkBackendRouterSocket;
  }

  createPublisherSocket(context: ZLinkBackendContext): ZLinkBackendPublisherSocket {
    return wrapSocket(zlink.createPubSocket(asNodeContext(context))) as unknown as ZLinkBackendPublisherSocket;
  }

  createSubscriberSocket(context: ZLinkBackendContext): ZLinkBackendSubscriberSocket {
    return wrapSocket(zlink.createSubSocket(asNodeContext(context))) as unknown as ZLinkBackendSubscriberSocket;
  }

  createReadablePoller(socket: ZLinkBackendSubscriberSocket): ZLinkBackendReadablePoller {
    return createNodeReadablePoller(socket);
  }
}

class ZLinkNodeStreamBackendAdapter implements ZLinkStreamBackendAdapter {
  createStreamSocket(context: ZLinkBackendContext): ZLinkBackendStreamSocket {
    const socket = zlink.createStreamSocket(asNodeContext(context));
    socket.options.recvMode = zlink.StreamRecvMode.Packet;
    return wrapSocket(socket) as unknown as ZLinkBackendStreamSocket;
  }

  createStreamPacket(): ZLinkBackendStreamPacket {
    return new zlink.StreamPacket() as ZLinkBackendStreamPacket;
  }

  createReadablePoller(socket: ZLinkBackendStreamSocket): ZLinkBackendReadablePoller {
    return createNodeReadablePoller(socket);
  }
}

class ZLinkNodeMonitoringBackendAdapter implements ZLinkMonitoringBackendAdapter {
  openSocketMonitor(socket: ZLinkBackendSocket): ZLinkBackendSocketMonitor {
    const nativeSocket = socket.nativeInstance as {
      monitorOpen(): ReturnType<ZLinkBindingModule['createDealerSocket']>['monitorOpen'] extends (...args: never[]) => infer T
        ? T
        : never;
    };
    return wrapMonitorSocket(nativeSocket.monitorOpen());
  }
}

class ZLinkNodeBackendContext implements ZLinkBackendContext {
  constructor(readonly nativeInstance: Context) {}

  shutdown(): void {
    this.nativeInstance.shutdown();
  }

  configureCoreHwm(options: ZLinkCoreHwmOptions | undefined): void {
    if (options === undefined) return;
    if (options.profile !== undefined) this.nativeInstance.options.coreHwmProfile = options.profile;
    if (options.memoryLimitBytes !== undefined) this.nativeInstance.options.coreHwmMemoryLimitBytes = options.memoryLimitBytes;
    if (options.budgetBytes !== undefined) this.nativeInstance.options.coreHwmBudgetBytes = options.budgetBytes;
  }

  getCoreHwmBudgetSnapshot(): CoreHwmBudgetSnapshot {
    return this.nativeInstance.getCoreHwmBudgetSnapshot();
  }

  resetCoreHwmBudgetMetrics(): void {
    this.nativeInstance.resetCoreHwmBudgetMetrics();
  }

  async dispose(): Promise<void> {
    // Terminal cleanup shuts the context down before terminating it, the same
    // sequence the .NET reference binding runs inside `Context.Dispose()`
    // (`zlink_ctx_shutdown` then `zlink_ctx_term`). Without the shutdown signal
    // termination can block forever waiting on the reaper even after every
    // socket this runtime owns has been closed.
    try {
      this.nativeInstance.shutdown();
    } catch (error) {
      if (!isContextTerminatedError(error)) {
        throw error;
      }
    }
    await closeWithBusyRetry(this.nativeInstance);
  }

  close(): void {
    this.nativeInstance.close();
  }
}

function asNodeContext(context: ZLinkBackendContext): Context {
  return context.nativeInstance as Context;
}

function createNodeReadablePoller(
  socket: { readonly nativeInstance: unknown }
): ZLinkBackendReadablePoller {
  const nativeSocket = socket.nativeInstance as {
    setReadableHandler(handler: () => void): void;
  };
  let disposed = false;
  let readable = false;
  let pending: {
    readonly promise: Promise<boolean>;
    readonly resolve: (readable: boolean) => void;
    readonly signal?: AbortSignal;
    readonly onAbort?: () => void;
  } | undefined;

  const settlePending = (value: boolean): void => {
    const current = pending;
    if (current === undefined) return;
    pending = undefined;
    if (current.signal !== undefined && current.onAbort !== undefined) {
      current.signal.removeEventListener('abort', current.onAbort);
    }
    current.resolve(value);
  };

  nativeSocket.setReadableHandler(() => {
    if (disposed) return;
    readable = true;
    settlePending(true);
  });

  return {
    wait(_timeoutMs: number): boolean {
      return !disposed && readable;
    },
    waitForReadable(signal?: AbortSignal): Promise<boolean> {
      if (disposed || signal?.aborted === true) return Promise.resolve(false);
      if (readable) return Promise.resolve(true);
      if (pending !== undefined) return pending.promise;

      let resolvePending!: (value: boolean) => void;
      const promise = new Promise<boolean>((resolve) => {
        resolvePending = resolve;
      });
      const onAbort = signal === undefined
        ? undefined
        : (): void => settlePending(false);
      pending = { promise, resolve: resolvePending, signal, onAbort };
      if (signal !== undefined && onAbort !== undefined) {
        signal.addEventListener('abort', onAbort, { once: true });
      }
      return promise;
    },
    markDrained(): void {
      readable = false;
    },
    dispose(): void {
      if (disposed) return;
      disposed = true;
      readable = false;
      settlePending(false);
    }
  };
}
