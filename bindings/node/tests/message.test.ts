// SPDX-License-Identifier: MPL-2.0

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');

test('message copy shares native payload and survives source close', () => {
  const source = zlink.Message.allocate(1024);
  const sourceView = source.data();
  sourceView.fill(0x2a);
  const copy = source.copy();
  const copyView = copy.data();

  assert.equal(source.refCount(), 2);
  assert.equal(copy.refCount(), 2);
  assert.equal(copy.size(), 1024);
  assert.equal(copyView[0], 0x2a);

  source.close();
  source.close();
  assert.equal(sourceView.byteLength, 1024);
  assert.equal(sourceView[0], 0x2a);
  assert.equal(copyView[1023], 0x2a);
  copy.close();
  copy.close();
  assert.equal(sourceView[1023], 0x2a);
  assert.equal(copyView[1023], 0x2a);
});

test('message copy releases an unexposed source frame immediately', () => {
  const source = zlink.Message.allocate(1024);
  const copy = source.copy();

  assert.equal(source.refCount(), 2);
  assert.equal(copy.refCount(), 2);

  source.close();
  assert.equal(copy.refCount(), 1);
  assert.equal(copy.size(), 1024);

  copy.close();
});

test('message move transfers payload, replaces destination, and leaves source empty', () => {
  const source = zlink.Message.allocate(1024);
  const sourceView = source.data();
  sourceView.fill(0x5a);
  const destination = zlink.Message.from('replaced');
  const replacedView = destination.data();

  source.move(destination);

  assert.equal(source.isEmpty(), true);
  assert.equal(source.refCount(), 1);
  assert.equal(sourceView.byteLength, 0);
  assert.equal(replacedView.byteLength, 0);
  assert.equal(destination.size(), 1024);
  assert.equal(destination.refCount(), 1);
  assert.equal(destination.data()[0], 0x5a);
  assert.equal(destination.data()[1023], 0x5a);

  source.close();
  destination.close();
});

test('message clone owns independent mutable payload storage', () => {
  const source = zlink.Message.allocate(1024);
  source.data().fill(0x33);
  const clone = source.clone();

  clone.data()[0] = 0x7c;
  assert.equal(source.data()[0], 0x33);
  assert.equal(clone.data()[0], 0x7c);
  assert.equal(source.refCount(), 1);
  assert.equal(clone.refCount(), 1);

  clone.close();
  assert.equal(source.data()[1023], 0x33);
  source.close();
});
