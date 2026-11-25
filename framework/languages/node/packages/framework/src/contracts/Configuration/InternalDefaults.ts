import { readFileSync } from 'node:fs';
import * as os from 'node:os';

/** Default complete header plus payload size accepted by a StreamNode. */
export const DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE = 64 * 1024;

/** service-wire 스키마가 정한 relocation chunk payload의 바이트 상한입니다. */
export const RELOCATION_STATE_CHUNK_DATA_MAX_BYTES = 67_108_864;

/** Default MeshNode pending activation limit (spec 03-mesh-node §5.1). */
export const DEFAULT_ACTIVATION_CONCURRENCY_LIMIT = 128;

/** Defaults shared by registration normalization and the runtime worker pool. */
export const DEFAULT_WORKER_MIN_THREADS = 0;
export const DEFAULT_WORKER_IDLE_TIMEOUT_MS = 30_000;

export function defaultWorkerMaxThreads(): number {
  return Math.max(2, os.availableParallelism());
}

export const ZLINK_RELOCATION_MAX_BLOB_REFERENCE_BYTES = 4096;

export const AUTHORITY_ENVELOPE_MAX_BYTES = 1024 * 1024;
export const CREATION_INTENT_MAX_BYTES = 1024 * 1024;
export const CREATION_TERMINAL_ENVELOPE_MAX_BYTES = 1024 * 1024;

export const DEFAULT_STREAM_HEARTBEAT_INTERVAL_MS = 1_000;
export const DEFAULT_STREAM_HEARTBEAT_TIMEOUT_MS = 5_000;
export const DEFAULT_STREAM_IDLE_TIMEOUT_MS = 0;

const CGROUP_V2_CPU_MAX = '/sys/fs/cgroup/cpu.max';
const CGROUP_V1_CPU_QUOTA = '/sys/fs/cgroup/cpu/cpu.cfs_quota_us';
const CGROUP_V1_CPU_PERIOD = '/sys/fs/cgroup/cpu/cpu.cfs_period_us';

export function nodeEffectiveProcessorCount(
  explicitExecutorMaximum?: number,
  runtimeConstrainedLogicalCount: number = os.availableParallelism(),
  readSystemText: (path: string) => string | undefined = readSystemFile
): bigint {
  const candidates = [
    positiveSafeInteger(runtimeConstrainedLogicalCount),
    cgroupQuotaProcessorCount(readSystemText),
    positiveSafeInteger(explicitExecutorMaximum)
  ].filter((candidate): candidate is number => candidate !== undefined);
  return BigInt(candidates.length === 0 ? 1 : Math.min(...candidates));
}

function cgroupQuotaProcessorCount(
  readSystemText: (path: string) => string | undefined
): number | undefined {
  const cpuMax = readSystemText(CGROUP_V2_CPU_MAX)?.trim().split(/\s+/u);
  if (cpuMax !== undefined && cpuMax.length >= 2 && cpuMax[0] !== 'max') {
    const count = quotaProcessorCount(cpuMax[0]!, cpuMax[1]!);
    if (count !== undefined) return count;
  }
  const quota = readSystemText(CGROUP_V1_CPU_QUOTA)?.trim();
  const period = readSystemText(CGROUP_V1_CPU_PERIOD)?.trim();
  return quota === undefined || period === undefined
    ? undefined
    : quotaProcessorCount(quota, period);
}

function quotaProcessorCount(quotaText: string, periodText: string): number | undefined {
  try {
    const quota = BigInt(quotaText);
    const period = BigInt(periodText);
    if (quota <= 0n || period <= 0n) return undefined;
    const floored = quota / period;
    const count = floored < 1n ? 1n : floored;
    return count > BigInt(Number.MAX_SAFE_INTEGER) ? Number.MAX_SAFE_INTEGER : Number(count);
  } catch {
    return undefined;
  }
}

function positiveSafeInteger(value: number | undefined): number | undefined {
  return value !== undefined && Number.isSafeInteger(value) && value > 0 ? value : undefined;
}

function readSystemFile(path: string): string | undefined {
  try {
    return readFileSync(path, 'utf8');
  } catch {
    return undefined;
  }
}
