import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from './framework-errors-internal';
export class ZLinkRuntimeAdmissionGate {
  private readonly meshes = new Map<string, ZLinkMeshAdmissionState>();

  get acceptsNewWork(): boolean {
    for (const state of this.meshes.values()) {
      if (state.sealed) return false;
    }
    return true;
  }

  register(meshName: string): void {
    if (!this.meshes.has(meshName)) {
      this.meshes.set(meshName, {
        sealed: false,
        sealedReason: ZLinkFrameworkInternalErrorKind.RequestRejected,
        active: 0,
        waiters: [],
        waiterHead: 0,
        waiterCount: 0
      });
    }
  }

  accepts(meshName: string): boolean {
    return !this.requireState(meshName).sealed;
  }

  pending(meshName: string): number {
    return this.requireState(meshName).active;
  }

  seal(
    meshName: string,
    reason: ZLinkFrameworkInternalErrorKind = ZLinkFrameworkInternalErrorKind.RequestRejected
  ): void {
    const state = this.requireState(meshName);
    state.sealed = true;
    state.sealedReason = reason;
  }

  close(): void {
    for (const state of this.meshes.values()) {
      state.sealed = true;
      state.sealedReason = ZLinkFrameworkInternalErrorKind.RuntimeShutdown;
    }
  }

  claim(meshName: string, operation: string): ZLinkApplicationWorkClaim {
    const state = this.requireState(meshName);
    if (!state.sealed) {
      state.active += 1;
      let closed = false;
      return {
        close: () => {
          if (closed) return;
          closed = true;
          state.active -= 1;
          if (state.active === 0) {
            let resolve: (() => void) | undefined;
            while ((resolve = takeWaiter(state)) !== undefined) resolve();
          }
        }
      };
    }
    throw createInternalFrameworkException(
      state.sealedReason,
      `${operation} was rejected because the framework is draining.`
    );
  }

  async run<T>(meshName: string, operation: string, work: () => Promise<T>): Promise<T> {
    const claim = this.claim(meshName, operation);
    try {
      return await work();
    } finally {
      claim.close();
    }
  }

  awaitZero(meshName: string, signal?: AbortSignal): Promise<void> {
    const state = this.requireState(meshName);
    if (state.active === 0) return Promise.resolve();
    if (signal?.aborted === true) return Promise.reject(signal.reason);
    return new Promise<void>((resolve, reject) => {
      const complete = () => {
        signal?.removeEventListener('abort', abort);
        resolve();
      };
      const abort = () => {
        const index = state.waiters.indexOf(complete, state.waiterHead);
        if (index >= 0) {
          state.waiters[index] = undefined;
          state.waiterCount -= 1;
          advanceWaiterHead(state);
          compactWaiters(state);
        }
        reject(signal?.reason);
      };
        state.waiters.push(complete);
        state.waiterCount += 1;
      signal?.addEventListener('abort', abort, { once: true });
    });
  }

  requireRequest(operation: string, meshName?: string): void {
    if (meshName === undefined) {
      if (this.acceptsNewWork) return;
    } else if (this.accepts(meshName)) {
      return;
    }
    throw createInternalFrameworkException(
      meshName === undefined
        ? this.firstSealedReason()
        : this.requireState(meshName).sealedReason,
      `${operation} was rejected because the framework is draining.`
    );
  }

  requireActorCreate(actorId: string, meshName?: string): void {
    if (meshName === undefined) {
      if (this.acceptsNewWork) return;
    } else if (this.accepts(meshName)) {
      return;
    }
    const reason = meshName === undefined
      ? this.firstSealedReason()
      : this.requireState(meshName).sealedReason;
    throw createInternalFrameworkException(
      reason === ZLinkFrameworkInternalErrorKind.RuntimeShutdown
        ? ZLinkFrameworkInternalErrorKind.RuntimeShutdown
        : ZLinkFrameworkInternalErrorKind.ActorCreateRejected,
      `Actor '${actorId}' create request was rejected because the framework is draining.`
    );
  }

  private requireState(meshName: string): ZLinkMeshAdmissionState {
    const state = this.meshes.get(meshName);
    if (state !== undefined) return state;
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.RouteNotConnected,
      `RouteMesh '${meshName}' is not registered.`
    );
  }

  private firstSealedReason(): ZLinkFrameworkInternalErrorKind {
    for (const state of this.meshes.values()) {
      if (state.sealed) return state.sealedReason;
    }
    return ZLinkFrameworkInternalErrorKind.RuntimeShutdown;
  }
}

export interface ZLinkApplicationWorkClaim {
  close(): void;
}

interface ZLinkMeshAdmissionState {
  sealed: boolean;
  sealedReason: ZLinkFrameworkInternalErrorKind;
  active: number;
  readonly waiters: Array<(() => void) | undefined>;
  waiterHead: number;
  waiterCount: number;
}

function takeWaiter(state: ZLinkMeshAdmissionState): (() => void) | undefined {
  advanceWaiterHead(state);
  const waiter = state.waiters[state.waiterHead];
  if (waiter === undefined) return undefined;
  state.waiters[state.waiterHead] = undefined;
  state.waiterHead += 1;
  state.waiterCount -= 1;
  compactWaiters(state);
  return waiter;
}

function advanceWaiterHead(state: ZLinkMeshAdmissionState): void {
  while (state.waiterHead < state.waiters.length && state.waiters[state.waiterHead] === undefined) {
    state.waiterHead += 1;
  }
}

function compactWaiters(state: ZLinkMeshAdmissionState): void {
  if (state.waiterCount === 0) {
    state.waiters.length = 0;
    state.waiterHead = 0;
  } else if (state.waiterHead >= 1024 && state.waiterHead * 2 >= state.waiters.length) {
    state.waiters.splice(0, state.waiterHead);
    state.waiterHead = 0;
  }
}
