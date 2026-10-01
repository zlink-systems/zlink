import * as os from 'node:os';

/** Default complete header plus payload size accepted by a StreamNode. */
export const DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE = 64 * 1024;

/** service-wire 스키마가 정한 relocation chunk payload의 바이트 상한입니다. */
export const RELOCATION_STATE_CHUNK_DATA_MAX_BYTES = 67_108_864;

/** Default MeshNode pending activation limit (spec 03-mesh-node 짠5.1). */
export const DEFAULT_ACTIVATION_CONCURRENCY_LIMIT = 128;

/** Defaults shared by registration normalization and the runtime worker pool. */
export const DEFAULT_WORKER_MIN_THREADS = 0;
export const DEFAULT_WORKER_IDLE_TIMEOUT_MS = 30_000;

export function defaultWorkerMaxThreads(): number {
  return Math.max(2, os.availableParallelism());
}
