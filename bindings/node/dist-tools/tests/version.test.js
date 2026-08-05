'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('version matches core', () => {
    const versionFile = fs.readFileSync(path.join(process.cwd(), '..', '..', 'VERSION'), 'utf8');
    const major = Number(versionFile.match(/^LIBZLINK_VERSION_MAJOR=(\d+)$/m)[1]);
    const minor = Number(versionFile.match(/^LIBZLINK_VERSION_MINOR=(\d+)$/m)[1]);
    const patch = Number(versionFile.match(/^LIBZLINK_VERSION_PATCH=(\d+)$/m)[1]);
    assert.deepEqual(zlink.version(), [major, minor, patch]);
});
test('legacy compatibility socket stays out of the aligned public api', () => {
    assert.equal(zlink.Socket, undefined);
    assert.equal(zlink.BaseSocket, undefined);
});
test('dedicated option helpers cover routing id and generic option access', () => {
    const ctx = zlink.createContext();
    const dealer = zlink.createDealerSocket(ctx);
    const routingId = zlink.RoutingId.from(Buffer.from('dealer-1'));
    dealer.setRoutingId(routingId);
    assert.equal(dealer.getRoutingId().toBytes().toString(), 'dealer-1');
    dealer.close();
    ctx.close();
});
test('stream compat helpers stay off the canonical stream surface', () => {
    const ctx = zlink.createContext();
    const stream = zlink.createStreamSocket(ctx);
    assert.equal(stream.streamAttach, undefined);
    assert.equal(stream.streamDetach, undefined);
    assert.equal(typeof stream.setPacketHandler, 'function');
    assert.equal(stream.onReceive, undefined);
    assert.equal(typeof stream.send, 'function');
    assert.equal(zlink.Socket, undefined);
    assert.equal(zlink.StreamDispatchMode, undefined);
    stream.close();
    ctx.close();
});
