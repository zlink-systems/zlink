// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
exports.STOP_TOKEN_BYTES = exports.STOP_TOKEN_STRING = void 0;
exports.isStopToken = isStopToken;
exports.isStopTokenParts = isStopTokenParts;
// Wire-level shutdown token shared by all Node perf patterns.
//
// PERF_SINGLE_TEST_POLICY § 1.4 / PERF_MULTI_TEST_POLICY § 1.3.1 mandate
// using a single sentinel message instead of `senderDone` flags + short
// polling cadences. The sender emits this token once at phase end; the
// receiver, waiting on `poller.wait(events, -1)`, drains in-flight messages and exits when
// it sees the token. This mirrors `k_stop_token` / `is_stop_token` in
// `bindings/cpp/perf/single/common/perf_single_common.hpp` and keeps
// the wire bytes identical across language bindings.
const STOP_TOKEN_STRING = '__zlink_perf_stop__';
exports.STOP_TOKEN_STRING = STOP_TOKEN_STRING;
const STOP_TOKEN_BYTES = Buffer.from(STOP_TOKEN_STRING);
exports.STOP_TOKEN_BYTES = STOP_TOKEN_BYTES;
function isStopToken(buf) {
    if (!buf) {
        return false;
    }
    if (buf.length !== STOP_TOKEN_BYTES.length) {
        return false;
    }
    if (Buffer.isBuffer(buf)) {
        return buf.equals(STOP_TOKEN_BYTES);
    }
    if (buf instanceof Uint8Array) {
        return Buffer.from(buf.buffer, buf.byteOffset, buf.byteLength).equals(STOP_TOKEN_BYTES);
    }
    return false;
}
function isStopTokenParts(parts) {
    if (!parts || parts.length !== 1) {
        return false;
    }
    const part = parts[0];
    if (part && typeof part.size === 'function' && part.size() !== STOP_TOKEN_BYTES.length) {
        return false;
    }
    if ((Buffer.isBuffer(part) || part instanceof Uint8Array)
        && part.length !== STOP_TOKEN_BYTES.length) {
        return false;
    }
    const data = typeof part?.data === 'function' ? part.data() : part;
    return isStopToken(data);
}
module.exports = {
    STOP_TOKEN_STRING,
    STOP_TOKEN_BYTES,
    isStopToken,
    isStopTokenParts,
};
