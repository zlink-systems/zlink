'use strict';
Object.defineProperty(exports, "__esModule", { value: true });
const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
test('public root exports raw capabilities without service projections', () => {
    for (const name of [
        'createContext',
        'createPairSocket',
        'createRouterSocket',
        'createStreamSocket',
        'createPoller',
        'createTimer',
        'Received',
        'Message'
    ]) {
        assert.notEqual(zlink[name], undefined, name);
    }
});
test('package exports block internal addon and runtime modules', () => {
    assert.throws(() => require('@zlink-systems/zlink/dist/zlink/runtime/native/native'), { code: 'ERR_PACKAGE_PATH_NOT_EXPORTED' });
});
