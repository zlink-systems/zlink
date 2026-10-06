// SPDX-License-Identifier: MPL-2.0
'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const { waitForConnectionReadyCount } = require('../perf/multi/perf_multi_runtime');
function monitoredSocket(counts) {
    const events = counts.map((value) => ({
        event: zlink.MonitorEventType.ConnectionReady,
        value: BigInt(value)
    }));
    let closed = false;
    return {
        monitorOpen() {
            return {
                recv: () => events.shift() || null,
                close: () => { closed = true; }
            };
        },
        isClosed: () => closed
    };
}
test('ready count snapshots do not admit missing connections', async () => {
    const socket = monitoredSocket([1, 1, 1, 0]);
    await assert.rejects(waitForConnectionReadyCount(socket, 3, null, 20), /\(0\/3\)/);
    assert.equal(socket.isClosed(), true);
});
test('the current ready count admits all requested connections', async () => {
    const socket = monitoredSocket([3]);
    await waitForConnectionReadyCount(socket, 3, null, 20);
    assert.equal(socket.isClosed(), true);
});
