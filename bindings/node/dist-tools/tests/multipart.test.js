'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('pair sockets send and receive multipart through canonical api', () => {
    const ctx = zlink.createContext();
    const left = zlink.createPairSocket(ctx);
    const right = zlink.createPairSocket(ctx);
    left.bind('inproc://multipart-contract');
    right.connect('inproc://multipart-contract');
    right.send().message('a').message(Buffer.from('b')).submit();
    const received = new zlink.Received();
    left.recv(received);
    assert.deepEqual(received.parts.map((part) => part.data().toString()), ['a', 'b']);
    right.close();
    left.close();
    ctx.close();
});
