import type { MonitorEvent } from '@zlink-systems/zlink';
import type { ZLinkBackendSocketMonitor, ZLinkBackendSocketMonitorEvent } from '../contracts';

export function wrapMonitorSocket(
  nativeInstance: { close(): void; recv(flags?: number): MonitorEvent | null }
): ZLinkBackendSocketMonitor {
  return {
    nativeInstance,
    async dispose(): Promise<void> {
      nativeInstance.close();
    },
    recv() {
      const event = nativeInstance.recv();
      return event === null ? undefined as never : toBackendMonitorEvent(event);
    },
    onEvent(handler): void {
      (nativeInstance as unknown as { onEvent(value: (event: MonitorEvent) => void): void })
        .onEvent(event => handler(toBackendMonitorEvent(event)));
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
