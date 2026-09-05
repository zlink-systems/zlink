"use strict";
var __importDefault = (this && this.__importDefault) || function (mod) {
    return (mod && mod.__esModule) ? mod : { "default": mod };
};
Object.defineProperty(exports, "__esModule", { value: true });
// SPDX-License-Identifier: MPL-2.0
const node_test_1 = __importDefault(require("node:test"));
const strict_1 = __importDefault(require("node:assert/strict"));
const zlink = require('@zlink-systems/zlink');
(0, node_test_1.default)('multipart receive reuse preserves captured reply routes and collection identities', async () => {
    const ctx = zlink.createContext();
    const router = zlink.createRouterSocket(ctx);
    const dealers = [zlink.createDealerSocket(ctx), zlink.createDealerSocket(ctx)];
    const received = new zlink.Received();
    const saved = [];
    const requests = [];
    try {
        router.bind('inproc://routed-receive-ownership');
        for (let index = 0; index < dealers.length; ++index) {
            const dealer = dealers[index];
            dealer.setRoutingId(zlink.RoutingId.from(`peer-${index}`));
            dealer.connect('inproc://routed-receive-ownership');
            requests.push(dealer.request().message(`body-${index}`).message('').timeout(1000).submit());
            strict_1.default.equal(router.recv(received), true);
            const parts = received.parts;
            strict_1.default.ok(Object.isFrozen(parts));
            strict_1.default.equal(parts.length, 2);
            for (const part of parts) {
                strict_1.default.equal(part.getProperty('Routing-Id'), `peer-${index}`);
                strict_1.default.equal(part.getProperty('Identity'), `peer-${index}`);
            }
            saved.push({ parts, route: received.routingId, token: received.replyToken,
                reply: received.reply().message(`reply-${index}`), view: parts[0].data() });
        }
        strict_1.default.notStrictEqual(saved[0].parts, saved[1].parts);
        strict_1.default.equal(saved[0].parts.length, 2);
        strict_1.default.equal(saved[0].route.toBytes().toString(), 'peer-0');
        strict_1.default.equal(saved[0].view.toString(), 'body-0');
        strict_1.default.equal(saved[0].token.equals(saved[1].token), false);
        strict_1.default.equal(saved[0].token.equals(saved[0].token), true);
        strict_1.default.equal(saved[0].token.hashCode(), saved[0].token.hashCode());
        strict_1.default.throws(() => router.reply(saved[0].route, Object.create(zlink.ReplyToken.prototype)), TypeError);
        strict_1.default.throws(() => router.reply(saved[0].route, {}), TypeError);
        saved[1].reply.submit();
        saved[0].reply.submit();
        const replies = await Promise.all(requests);
        try {
            strict_1.default.deepEqual(replies.map(parts => parts[0].getString()), ['reply-0', 'reply-1']);
        }
        finally {
            for (const parts of replies)
                for (const part of parts)
                    part.close();
        }
    }
    finally {
        received.close();
        for (const dealer of dealers)
            dealer.close();
        router.close();
        ctx.close();
    }
});
