// SPDX-License-Identifier: MPL-2.0
'use strict';

// Bench spec section 6: a 29-byte measurement header at the front of the payload
// body. The layout is identical in every language; a language that invents its
// own layout cannot have its cells put next to another language's.
//
//   0  4  magic 0x5A4C4E4B ("ZLNK")
//   4  4  run id
//   8  1  phase (0 warmup, 1 active)
//   9  4  payload size
//  13  8  sequence
//  21  8  send timestamp ns

const HEADER_SIZE = 29;
const MAGIC = 0x5a4c4e4b;

const PHASE_WARMUP = 0;
const PHASE_ACTIVE = 1;

/**
 * Monotonic nanoseconds. `process.hrtime.bigint()` is uv_hrtime, i.e.
 * CLOCK_MONOTONIC, so two processes on this machine read the same timeline --
 * which is what makes the server-side receive latency of `send-saturation`
 * meaningful. Wall clock is not used: this host's wall clock jumps by seconds
 * (project note env-wsl-wall-clock-jumps).
 */
function nowNs() {
  return process.hrtime.bigint();
}

function createPayloadBytes(payloadSize, runId, phase, sequence) {
  if (payloadSize < HEADER_SIZE) {
    throw new RangeError(`payload size must be at least ${HEADER_SIZE} bytes`);
  }
  const bytes = Buffer.allocUnsafe(payloadSize);
  bytes.fill(0xab);
  stamp(bytes, runId, phase, payloadSize, sequence);
  return bytes;
}

function stamp(bytes, runId, phase, payloadSize, sequence) {
  bytes.writeUInt32LE(MAGIC, 0);
  bytes.writeUInt32LE(runId >>> 0, 4);
  bytes.writeUInt8(phase, 8);
  bytes.writeUInt32LE(payloadSize >>> 0, 9);
  bytes.writeBigUInt64LE(BigInt(sequence), 13);
  bytes.writeBigInt64LE(nowNs(), 21);
  return bytes;
}

function decode(bytes) {
  if (!bytes || bytes.length < HEADER_SIZE) return null;
  if (bytes.readUInt32LE(0) !== MAGIC) return null;
  return {
    runId: bytes.readUInt32LE(4),
    phase: bytes.readUInt8(8),
    payloadSize: bytes.readUInt32LE(9),
    sequence: bytes.readBigUInt64LE(13),
    sentTimestampNs: bytes.readBigInt64LE(21)
  };
}

/** G2: the request patterns validate the header that came back. */
function isExpected(header, runId, phase, payloadSize, sequence) {
  return header !== null
    && header.runId === runId
    && header.phase === phase
    && header.payloadSize === payloadSize
    && header.sequence === BigInt(sequence);
}

module.exports = {
  HEADER_SIZE,
  MAGIC,
  PHASE_WARMUP,
  PHASE_ACTIVE,
  nowNs,
  createPayloadBytes,
  stamp,
  decode,
  isExpected
};
