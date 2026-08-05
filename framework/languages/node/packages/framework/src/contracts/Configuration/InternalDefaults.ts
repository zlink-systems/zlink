import * as os from 'node:os';

/** Default complete header plus payload size accepted by a StreamNode. */
export const DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE = 64 * 1024;

/** Defaults shared by registration normalization and the runtime worker pool. */
export const DEFAULT_WORKER_MIN_THREADS = 0;
export const DEFAULT_WORKER_IDLE_TIMEOUT_MS = 30_000;
export const DEFAULT_WORKER_QUEUE_LENGTH = 1024;

export function defaultWorkerMaxThreads(): number {
  return Math.max(2, os.availableParallelism());
}
