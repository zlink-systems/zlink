// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
process.env.ZLINK_NODE_TEST_HOOKS = '1';
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_path_1 = __importDefault(require("node:path"));
const addonPath = node_path_1.default.resolve(__dirname, '../../build/Release/zlink.node');
const native = require(addonPath);
const expectedKeys = ['result', 'terminalErrno', 'token'];
function nextTurn() {
    return new Promise((resolve) => setImmediate(resolve));
}
async function waitForCompletion(events) {
    for (let turn = 0; turn < 20 && events.length === 0; turn += 1) {
        await nextTurn();
    }
}
(0, node_test_1.default)('send completion handoff owns every terminal path exactly once', async () => {
    const direct = native.testSendCompletionOperationPath(0);
    strict_1.default.deepEqual(Object.keys(direct ?? {}).sort(), expectedKeys);
    strict_1.default.equal(direct?.token, 42n);
    strict_1.default.equal(direct?.result, 0);
    const early = native.testSendCompletionOperationPath(1);
    strict_1.default.deepEqual(Object.keys(early ?? {}).sort(), expectedKeys);
    strict_1.default.equal(early?.token, 42n);
    strict_1.default.equal(early?.result, 0);
    const queued = [];
    native.testSendCompletionOperationPath(2, (event) => queued.push(event));
    await waitForCompletion(queued);
    strict_1.default.equal(queued.length, 1);
    strict_1.default.deepEqual(Object.keys(queued[0]).sort(), expectedKeys);
    strict_1.default.equal(queued[0].token, 42n);
    strict_1.default.equal(queued[0].result, 0);
    const rejected = [];
    native.testSendCompletionOperationPath(3, (event) => rejected.push(event));
    await waitForCompletion(rejected);
    strict_1.default.equal(rejected.length, 1, 'the queue-filling sentinel must still be delivered');
    strict_1.default.equal(rejected[0].token, 41n);
    strict_1.default.equal(rejected.some((event) => event.token === 42n), false, 'the operation rejected by the full TSFN queue must not be delivered');
});
