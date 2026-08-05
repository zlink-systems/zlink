// SPDX-License-Identifier: MPL-2.0

export * from './zlink/contracts';

import {
  has as runtimeHas,
  multipartClose as runtimeMultipartClose,
  proxy as runtimeProxy,
  proxySteerable as runtimeProxySteerable,
  sleep as runtimeSleep,
  strerror as runtimeStrerror,
  version as runtimeVersion,
} from './zlink/runtime/core/runtime_info';
import { RuntimeContext } from './zlink/runtime/core/context';
import {
  RuntimeAtomicCounter,
  RuntimePoller,
  RuntimePollEvents,
  RuntimeStopwatch,
  RuntimeThread,
  RuntimeTimer,
} from './zlink/runtime/eventing';
import {
  RuntimeDealerSocket,
  RuntimePairSocket,
  RuntimePubSocket,
  RuntimeRouterSocket,
  RuntimeStreamSocket,
  RuntimeSubSocket,
  RuntimeXPubSocket,
  RuntimeXSubSocket,
} from './zlink/runtime/sockets';
import {
  asPublicContract,
  asRuntimeContext,
  asRuntimeSocket,
} from './zlink/runtime/public_bridge';
import type {
  AtomicCounter,
  Context,
  DealerSocket,
  PairSocket,
  Poller,
  PollEvents,
  PubSocket,
  RouterSocket,
  Stopwatch,
  StreamSocket,
  SubSocket,
  Thread,
  Timer,
  XPubSocket,
  XSubSocket,
} from './zlink/contracts';
import type { BaseSocket, Message } from './zlink/contracts';

export function createContext(): Context {
  return asPublicContract<Context>(new RuntimeContext());
}

export function createPairSocket(ctx: Context): PairSocket {
  return asPublicContract<PairSocket>(new RuntimePairSocket(asRuntimeContext(ctx)));
}

export function createPubSocket(ctx: Context): PubSocket {
  return asPublicContract<PubSocket>(new RuntimePubSocket(asRuntimeContext(ctx)));
}

export function createSubSocket(ctx: Context): SubSocket {
  return asPublicContract<SubSocket>(new RuntimeSubSocket(asRuntimeContext(ctx)));
}

export function createXPubSocket(ctx: Context): XPubSocket {
  return asPublicContract<XPubSocket>(new RuntimeXPubSocket(asRuntimeContext(ctx)));
}

export function createXSubSocket(ctx: Context): XSubSocket {
  return asPublicContract<XSubSocket>(new RuntimeXSubSocket(asRuntimeContext(ctx)));
}

export function createDealerSocket(ctx: Context): DealerSocket {
  return asPublicContract<DealerSocket>(new RuntimeDealerSocket(asRuntimeContext(ctx)));
}

export function createRouterSocket(ctx: Context): RouterSocket {
  return asPublicContract<RouterSocket>(new RuntimeRouterSocket(asRuntimeContext(ctx)));
}

export function createStreamSocket(ctx: Context): StreamSocket {
  return asPublicContract<StreamSocket>(new RuntimeStreamSocket(asRuntimeContext(ctx)));
}

export function createPoller(): Poller {
  return asPublicContract<Poller>(new RuntimePoller());
}

export function createPollEvents(capacity: number): PollEvents {
  return asPublicContract<PollEvents>(new RuntimePollEvents(capacity));
}

export function createTimer(): Timer {
  return asPublicContract<Timer>(new RuntimeTimer());
}

export function createThread(handler: () => void): Thread {
  return asPublicContract<Thread>(new RuntimeThread(handler));
}

export function createStopwatch(): Stopwatch {
  return asPublicContract<Stopwatch>(new RuntimeStopwatch());
}

export function createAtomicCounter(initialValue = 0): AtomicCounter {
  return asPublicContract<AtomicCounter>(new RuntimeAtomicCounter(initialValue));
}

export function version(): [number, number, number] {
  return runtimeVersion();
}

export function strerror(code: number): string {
  return runtimeStrerror(code);
}

export function has(capability: string): boolean {
  return runtimeHas(capability);
}

export function proxy(frontend: BaseSocket, backend: BaseSocket, capture?: BaseSocket): void {
  runtimeProxy(
    asRuntimeSocket(frontend),
    asRuntimeSocket(backend),
    capture ? asRuntimeSocket(capture) : undefined
  );
}

export function proxySteerable(
  frontend: BaseSocket,
  backend: BaseSocket,
  capture: BaseSocket | null,
  control: BaseSocket
): void {
  runtimeProxySteerable(
    asRuntimeSocket(frontend),
    asRuntimeSocket(backend),
    capture ? asRuntimeSocket(capture) : undefined,
    asRuntimeSocket(control)
  );
}

export function sleep(seconds: number): void {
  runtimeSleep(seconds);
}

export function multipartClose(parts: Message[]): void {
  runtimeMultipartClose(parts);
}
