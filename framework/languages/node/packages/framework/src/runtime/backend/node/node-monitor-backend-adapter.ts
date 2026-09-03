import type { MonitorEvent } from '@zlink-systems/zlink';
import type { ZLinkBackendSocketMonitor, ZLinkBackendSocketMonitorEvent } from '../contracts';
import { zlink } from './node-backend-adapter-support';

export function wrapMonitorSocket(
  nativeInstance: { close(): void; recv(flags?: number): MonitorEvent | null }
): ZLinkBackendSocketMonitor {
  const handlers = new Set<(event: ZLinkBackendSocketMonitorEvent) => void>();
  return {
    nativeInstance,
    async dispose(): Promise<void> {
      handlers.clear();
      nativeInstance.close();
    },
    drain() {
      let count = 0;
      for (;;) {
        const event = nativeInstance.recv(zlink.RecvFlags.DontWait);
        if (event === null) return count;
        const value = toBackendMonitorEvent(event);
        for (const registered of handlers) registered(value);
        count += 1;
      }
    },
    onEvent(handler): void {
      handlers.add(handler);
    }
  };
}

function toBackendMonitorEvent(event: MonitorEvent): ZLinkBackendSocketMonitorEvent {
  return {
    nativeEvent: event.event,
    routingId: event.routingId ?? undefined,
    localAddr: event.localAddr,
    remoteAddr: event.remoteAddr,
    value: event.value
  };
}
