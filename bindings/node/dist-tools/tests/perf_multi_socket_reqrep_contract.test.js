// SPDX-License-Identifier: MPL-2.0
'use strict';
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const node_fs_1 = __importDefault(require("node:fs"));
const node_path_1 = __importDefault(require("node:path"));
const zlink = require('@zlink-systems/zlink');
const source = node_fs_1.default.readFileSync(node_path_1.default.resolve(__dirname, '../../perf/multi/perf_multi_socket_reqrep.ts'), 'utf8');
function nextTurn() {
    return new Promise((resolve) => setImmediate(resolve));
}
(0, node_test_1.default)('multi REQREP keeps one completion poller progressing during active and drain turns', () => {
    strict_1.default.match(source, /completionPoller = zlink\.createPoller\(\)/);
    strict_1.default.match(source, /completionPoller\.add\(sockets\[index\], pollEvents\(POLLCOMPLETION\), index\)/);
    strict_1.default.equal((source.match(/waitPollerOne\(completionPoller, completionEvents, waitMs\)/g) ?? []).length, 1, 'the active submit turn must perform one completion poll');
    strict_1.default.match(source, /while \(\(pending\.size > 0 \|\| blocked\.size > 0\)[\s\S]*?waitPollerOne\(/);
    strict_1.default.doesNotMatch(source, /Promise\.race\(blocked\.values\(\)\)/);
});
(0, node_test_1.default)('one completion-only poller settles concurrent requests from multiple sockets', async () => {
    const context = zlink.createContext();
    const server = zlink.createRouterSocket(context);
    const clients = Array.from({ length: 4 }, () => zlink.createDealerSocket(context));
    const completionPoller = zlink.createPoller();
    const completionEvents = zlink.createPollEvents(clients.length);
    const received = new zlink.Received();
    const endpoint = `inproc://node-perf-multi-reqrep-${process.pid}`;
    try {
        server.bind(endpoint);
        clients.forEach((client, index) => {
            client.setRoutingId(zlink.RoutingId.from(Buffer.from(`CLIENT-${index}`)));
            client.connect(endpoint);
            completionPoller.add(client, [zlink.PollEventFlag.PollCompletion], index);
        });
        const expectedReplies = 32;
        let settledReplies = 0;
        const replies = [];
        for (let sequence = 0; sequence < expectedReplies; sequence += 1) {
            const client = clients[sequence % clients.length];
            const expected = `request-${sequence}`;
            const submission = client.request()
                .message(expected)
                .timeout(1_000)
                .submit();
            replies.push(submission.reply.then((parts) => {
                try {
                    strict_1.default.equal(parts[0].getString(), expected);
                    settledReplies += 1;
                }
                finally {
                    parts.forEach((part) => part.close());
                }
            }));
        }
        let serverReplies = 0;
        const deadline = Date.now() + 5_000;
        while (settledReplies < expectedReplies && Date.now() < deadline) {
            while (server.recv(received, zlink.RecvFlags.DontWait)) {
                received.reply().message(received.parts[0]).submit();
                received.close();
                serverReplies += 1;
            }
            completionPoller.wait(completionEvents, 0);
            await nextTurn();
        }
        strict_1.default.equal(serverReplies, expectedReplies);
        strict_1.default.equal(settledReplies, expectedReplies);
        await Promise.all(replies);
    }
    finally {
        received.close();
        completionEvents.close();
        completionPoller.close();
        clients.forEach((client) => client.close());
        server.close();
        context.close();
    }
});
