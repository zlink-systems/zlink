// SPDX-License-Identifier: MPL-2.0

'use strict';

const METRIC_MAGIC = 0x5a4c4e4b;
const HEADER_SIZE = 29;
const BASE_EPOCH_NS = BigInt(Date.now()) * 1_000_000n;
const BASE_HR_NS = process.hrtime.bigint();
const PRIMARY_METRICS = ['throughput', 'bandwidth', 'latency', 'latency_p95', 'latency_p99'];

function warnResultLine(message) {
  console.error(`warning: ${message}`);
}

function currentEpochNs() {
  return BASE_EPOCH_NS + (process.hrtime.bigint() - BASE_HR_NS);
}

function createPayload(size) {
  if (!Number.isInteger(size) || size < HEADER_SIZE) {
    throw new Error(`invalid payload size: ${size}`);
  }
  const payload = Buffer.alloc(size);
  for (let i = HEADER_SIZE; i < payload.length; i += 1) {
    payload[i] = 0x61 + (i % 23);
  }
  return payload;
}

function applyMetricHeader(buffer, values) {
  const sentTsNs = values.sentTsNs ?? currentEpochNs();
  buffer.writeUInt32LE(METRIC_MAGIC, 0);
  buffer.writeUInt32LE(values.runId >>> 0, 4);
  buffer.writeUInt8(values.phase & 0xff, 8);
  buffer.writeUInt32LE(values.msgSize >>> 0, 9);
  buffer.writeBigUInt64LE(BigInt(values.seq ?? 0), 13);
  buffer.writeBigInt64LE(BigInt(sentTsNs), 21);
}

function stampPayload(buffer, values) {
  applyMetricHeader(buffer, {
    phase: values.phase,
    runId: values.runId,
    msgSize: values.msgSize,
    seq: values.seq,
    sentTsNs: values.sentTsNs
  });
}

function decodeMetricHeader(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < HEADER_SIZE) {
    return null;
  }
  if (buffer.readUInt32LE(0) !== METRIC_MAGIC) {
    return null;
  }
  return {
    magic: METRIC_MAGIC,
    runId: buffer.readUInt32LE(4),
    phase: buffer.readUInt8(8),
    msgSize: buffer.readUInt32LE(9),
    seq: buffer.readBigUInt64LE(13),
    sentTsNs: buffer.readBigInt64LE(21)
  };
}

// C parity: bindings/c/perf/single/common/perf_single_one_way.hpp
// recv_single_part_header_flags (~95-113) and perf_single_metric_header.hpp
// decode_header (~106-119) decode the metric header strictly at byte 0 of
// the first part and reject any payload whose size does not match the
// expected payload size. No magic scanning / offset search. The same
// strict contract applies to multi PUBSUB/SPOT
// (perf_multi_client_helpers.hpp recv_one_message_header ~621-624 decodes
// at offset 0; the capture scan is STREAM-raw-only and unused here).
function decodeMetricHeaderFromParts(parts, expectedSize) {
  const enforceSize = Number.isFinite(expectedSize) && expectedSize > 0;
  for (const part of parts || []) {
    if (!part || typeof part.data !== 'function') {
      continue;
    }
    const data = part.data();
    if (enforceSize && data.length !== expectedSize) {
      return null;
    }
    const header = decodeMetricHeader(data);
    if (header) {
      return header;
    }
  }
  return null;
}

function latencyNsFromPayload(buffer, receivedAtNs = currentEpochNs()) {
  const header = decodeMetricHeader(buffer);
  if (!header) {
    return 0;
  }
  return Number(receivedAtNs - header.sentTsNs);
}

function percentile(sortedValues, q) {
  if (sortedValues.length === 0) {
    return 0;
  }
  const index = Math.min(
    sortedValues.length - 1,
    Math.max(0, Math.ceil(sortedValues.length * q) - 1)
  );
  return sortedValues[index];
}

function computeMetrics(
  latenciesNs,
  durationSeconds,
  msgSize,
  bandwidthMultiplier = 1,
  throughputCount = latenciesNs.length
) {
  const count = latenciesNs.length;
  const effectiveCount = Number.isFinite(throughputCount) && throughputCount >= 0
    ? throughputCount
    : count;
  const throughput = durationSeconds > 0 ? effectiveCount / durationSeconds : 0;
  const bandwidth = throughput * msgSize * bandwidthMultiplier / 1_000_000;
  const sorted = latenciesNs.slice().sort((a, b) => a - b);
  const latency = count > 0 ? sorted.reduce((sum, value) => sum + value, 0) / count : 0;
  const latencyP95 = percentile(sorted, 0.95);
  const latencyP99 = percentile(sorted, 0.99);

  return {
    throughput,
    bandwidth,
    latency: latency / 1_000_000,
    latency_p95: latencyP95 / 1_000_000,
    latency_p99: latencyP99 / 1_000_000
  };
}

function median(values) {
  const sorted = values.slice().sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  if ((sorted.length % 2) === 0) {
    return (sorted[mid - 1] + sorted[mid]) / 2;
  }
  return sorted[mid];
}

function medianMetrics(metricsList) {
  return {
    throughput: median(metricsList.map((item) => item.throughput)),
    bandwidth: median(metricsList.map((item) => item.bandwidth)),
    latency: median(metricsList.map((item) => item.latency)),
    latency_p95: median(metricsList.map((item) => item.latency_p95)),
    latency_p99: median(metricsList.map((item) => item.latency_p99))
  };
}

function isEchoPattern(pattern) {
  return pattern === 'DEALER_ROUTER_REQREP'
    || pattern === 'ROUTER_ROUTER_REQREP'
    || pattern === 'MULTI_DEALER_ROUTER'
    || pattern === 'MULTI_DEALER_ROUTER_REQREP'
    || pattern === 'MULTI_ROUTER_ROUTER'
    || pattern === 'MULTI_ROUTER_ROUTER_REQREP'
    || pattern === 'MULTI_STREAM'
    || pattern === 'MULTI_SPOT_REQREP'
    || pattern === 'MULTI_SPOT_SENDSEND';
}

function summarizeMetrics(
  pattern,
  transport,
  msgSize,
  latenciesNs,
  durationSeconds,
  libName = 'current',
  throughputCount = latenciesNs.length
) {
  if (!Number.isFinite(throughputCount) || throughputCount <= 0 || latenciesNs.length === 0) {
    throw new Error(`no measured messages for ${pattern} ${transport} ${msgSize}B`);
  }
  const metrics = computeMetrics(
    latenciesNs,
    durationSeconds,
    msgSize,
    isEchoPattern(pattern) ? 2 : 1,
    throughputCount
  );
  return Object.entries(metrics).map(([metric, value]) => {
    const formatted = value.toFixed(3);
    return `RESULT,${libName},${pattern},${transport},${msgSize},${metric},${formatted}`;
  });
}

function metricLines(pattern, transport, msgSize, metrics, libName = 'current') {
  return PRIMARY_METRICS.map((metric) => {
    const value = metrics[metric];
    if (typeof value !== 'number') {
      throw new Error(`missing metric ${metric} for ${pattern} ${transport} ${msgSize}B`);
    }
    return `RESULT,${libName},${pattern},${transport},${msgSize},${metric},${value.toFixed(3)}`;
  });
}

function collectPrimaryMetrics(pattern, msgSize, lines) {
  const metrics = {};
  for (const line of lines) {
    if (!line.startsWith('RESULT,')) {
      continue;
    }
    const parts = line.split(',');
    if (parts.length !== 7) {
      warnResultLine(`ignoring malformed RESULT line: ${line}`);
      continue;
    }
    if (parts[2] !== pattern || Number(parts[4]) !== msgSize) {
      continue;
    }
    if (PRIMARY_METRICS.includes(parts[5])) {
      if (Object.prototype.hasOwnProperty.call(metrics, parts[5])) {
        warnResultLine(
          `duplicate RESULT metric for ${pattern} ${msgSize}B ${parts[5]}; using last value`
        );
      }
      metrics[parts[5]] = Number(parts[6]);
    }
  }

  return metrics;
}

function hasPrimaryMetricsFromResultLines(pattern, msgSize, lines) {
  const metrics = collectPrimaryMetrics(pattern, msgSize, lines);
  return PRIMARY_METRICS.every((metric) => typeof metrics[metric] === 'number');
}

function primaryMetricsFromResultLines(pattern, msgSize, lines) {
  const metrics = collectPrimaryMetrics(pattern, msgSize, lines);

  if (PRIMARY_METRICS.some((metric) => typeof metrics[metric] !== 'number')) {
    throw new Error(`missing primary metrics for ${pattern} size ${msgSize}`);
  }

  return metrics;
}

function sleepImmediate() {
  return new Promise((resolve) => setImmediate(resolve));
}

function sleepMillis(ms) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));
}

function createRunId(caseOrdinal = 1) {
  if (!Number.isInteger(caseOrdinal) || caseOrdinal <= 0) {
    throw new Error(`invalid run id ordinal: ${caseOrdinal}`);
  }
  return caseOrdinal >>> 0;
}

function createMetricCollector(config) {
  const latenciesNs = [];
  const runId = config.runId >>> 0;
  const msgSize = config.msgSize >>> 0;
  const activeStartNs = config.activeStartNs === undefined
    ? 0n
    : BigInt(config.activeStartNs);
  const activeStopNs = config.activeStopNs === undefined
    ? BigInt('0xffffffffffffffff')
    : BigInt(config.activeStopNs);
  const rttDivisor = config.roundTrip ? 2n : 1n;
  const latencySampleStride = Math.max(1, Number(config.latencySampleStride ?? 1) | 0);
  let accepted = 0;
  let rejected = 0;
  let closed = false;

  function shouldSampleLatency(nextAccepted) {
    return ((nextAccepted - 1) % latencySampleStride) === 0;
  }

  return {
    recordPayload(buffer, receivedAtNs) {
      if (closed || !Buffer.isBuffer(buffer) || buffer.length < HEADER_SIZE) {
        return;
      }
      if (buffer.readUInt32LE(0) !== METRIC_MAGIC) {
        return;
      }
      if ((buffer.readUInt32LE(4) >>> 0) !== runId
          || (buffer.readUInt32LE(9) >>> 0) !== msgSize) {
        rejected += 1;
        return;
      }
      if (buffer.readUInt8(8) !== 1) {
        return;
      }
      const recvTsNs = BigInt(receivedAtNs);
      if (recvTsNs < activeStartNs || recvTsNs > activeStopNs) {
        return;
      }
      const nextAccepted = accepted + 1;
      if (shouldSampleLatency(nextAccepted)) {
        const sentTsNs = buffer.readBigInt64LE(21);
        if (recvTsNs < sentTsNs) {
          rejected += 1;
          return;
        }
        accepted = nextAccepted;
        latenciesNs.push(Number((recvTsNs - sentTsNs) / rttDivisor));
      } else {
        accepted = nextAccepted;
      }
    },
    record(header, receivedAtNs) {
      if (!header || closed) {
        return;
      }
      if ((header.runId >>> 0) !== runId || (header.msgSize >>> 0) !== msgSize) {
        rejected += 1;
        return;
      }
      if (header.phase !== 1) {
        return;
      }
      const sentTsNs = BigInt(header.sentTsNs);
      const recvTsNs = BigInt(receivedAtNs);
      if (recvTsNs < activeStartNs || recvTsNs > activeStopNs) {
        return;
      }
      const nextAccepted = accepted + 1;
      if (shouldSampleLatency(nextAccepted)) {
        if (recvTsNs < sentTsNs) {
          rejected += 1;
          return;
        }
        accepted = nextAccepted;
        latenciesNs.push(Number((recvTsNs - sentTsNs) / rttDivisor));
      } else {
        accepted = nextAccepted;
      }
    },
    recordLatencyNs(latencyNs) {
      if (closed || typeof latencyNs !== 'number' || latencyNs < 0) {
        return;
      }
      const nextAccepted = accepted + 1;
      if (shouldSampleLatency(nextAccepted)) {
        accepted = nextAccepted;
        latenciesNs.push(latencyNs / Number(rttDivisor));
      } else {
        accepted = nextAccepted;
      }
    },
    async finish() {
      closed = true;
      return {
        latenciesNs,
        accepted,
        rejected
      };
    },
    close() {
      closed = true;
      return Promise.resolve();
    }
  };
}

module.exports = {
  HEADER_SIZE,
  METRIC_MAGIC,
  PRIMARY_METRICS,
  computeMetrics,
  createMetricCollector,
  createPayload,
  createRunId,
  decodeMetricHeader,
  decodeMetricHeaderFromParts,
  currentEpochNs,
  latencyNsFromPayload,
  medianMetrics,
  metricLines,
  hasPrimaryMetricsFromResultLines,
  primaryMetricsFromResultLines,
  sleepImmediate,
  sleepMillis,
  stampPayload,
  summarizeMetrics
};
