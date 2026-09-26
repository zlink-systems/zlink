// SPDX-License-Identifier: MPL-2.0

import test from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';

const zlink = require('@zlink-systems/zlink');
const nativeModule = require(path.resolve(__dirname, '../../dist/zlink/runtime/native/native.js'));

function intercept(method: string, replacement: (...args: unknown[]) => unknown): () => void {
  const original = nativeModule.requireNative;
  const native = original();
  nativeModule.requireNative = () => new Proxy({}, {
    get(_target, property) {
      return property === method ? replacement : native[property];
    }
  });
  return () => { nativeModule.requireNative = original; };
}

test('context close calls shutdown before term', () => {
  const context = zlink.createContext();
  const calls: string[] = [];
  const original = nativeModule.requireNative;
  const native = original();
  nativeModule.requireNative = () => new Proxy({}, {
    get(_target, property) {
      if (property === 'ctxShutdown' || property === 'ctxTerm') {
        return (...args: unknown[]) => {
          calls.push(String(property));
          return native[property](...args);
        };
      }
      return native[property];
    }
  });
  try {
    context.close();
    assert.deepEqual(calls, ['ctxShutdown', 'ctxTerm']);
  } finally {
    nativeModule.requireNative = original;
    context.close();
  }
});

test('busy poller destroy reports typed Busy and leaves poller valid', () => {
  const poller = zlink.createPoller();
  const original = nativeModule.requireNative();
  let busy = true;
  const restore = intercept('pollerDestroy', (...args: unknown[]) => {
    if (busy) {
      busy = false;
      throw Object.assign(new Error('poller busy'), { nativeErrno: 16 });
    }
    return original.pollerDestroy(...args);
  });
  try {
    assert.throws(() => poller.close(), (error: any) =>
      error instanceof zlink.CloseError && error.result === zlink.CloseResult.Busy);
    assert.equal(poller.size, 0);
    poller.close();
  } finally {
    restore();
    poller.close();
  }
});
