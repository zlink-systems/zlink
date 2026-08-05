// SPDX-License-Identifier: MPL-2.0

import { requireNative } from '../native/native';

interface RequestProgressState {
  refCount: number;
  poller: unknown;
  interval: NodeJS.Timeout;
}

const requestProgressByHandle = new Map<unknown, RequestProgressState>();
const externalProgressByHandle = new Map<unknown, number>();

export function startRequestProgress(handle: unknown): () => void {
  if ((externalProgressByHandle.get(handle) ?? 0) > 0) {
    return () => {};
  }
  const existing = requestProgressByHandle.get(handle);
  if (existing) {
    existing.refCount += 1;
    return () => releaseRequestProgress(handle);
  }

  const state: RequestProgressState = {
    refCount: 1,
    poller: requireNative().pollerNew(),
    interval: setInterval(() => {
      const current = requestProgressByHandle.get(handle);
      if (!current) {
        return;
      }
      try {
        requireNative().pollerWait(current.poller, 0);
      } catch {
        // Progress calls are best-effort. Completion still arrives through the native callback.
      }
    }, 1)
  };
  try {
    requireNative().pollerAdd(state.poller, handle, null, 32);
  } catch {
    clearInterval(state.interval);
    try {
      requireNative().pollerDestroy(state.poller);
    } catch {
    }
    return () => {};
  }
  state.interval.unref();
  requestProgressByHandle.set(handle, state);
  return () => releaseRequestProgress(handle);
}

export function acquireExternalRequestProgress(handle: unknown): void {
  externalProgressByHandle.set(
    handle,
    (externalProgressByHandle.get(handle) ?? 0) + 1
  );
}

export function releaseExternalRequestProgress(handle: unknown): void {
  const count = externalProgressByHandle.get(handle) ?? 0;
  if (count <= 1) {
    externalProgressByHandle.delete(handle);
    return;
  }
  externalProgressByHandle.set(handle, count - 1);
}

function releaseRequestProgress(handle: unknown): void {
  const state = requestProgressByHandle.get(handle);
  if (!state) {
    return;
  }
  state.refCount -= 1;
  if (state.refCount > 0) {
    return;
  }
  clearInterval(state.interval);
  try {
    requireNative().pollerRemove(state.poller, handle);
  } catch {
  }
  try {
    requireNative().pollerDestroy(state.poller);
  } catch {
  }
  requestProgressByHandle.delete(handle);
}
