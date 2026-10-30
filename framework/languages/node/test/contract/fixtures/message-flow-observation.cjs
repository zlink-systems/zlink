'use strict';

const assert = require('node:assert/strict');
const { BroadcastChannel } = require('node:worker_threads');
const { join } = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');

module.exports = function observeMessageFlow(delivery, { onLog, onSpan } = {}) {
  const previousOptions = process.env.NODE_OPTIONS;
  const previousChannel = process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL;
  const name = `zlink-message-flow-${process.pid}`;
  const preload = join(__dirname, 'message-flow-provider.cjs').replace(/\\/g, '/');
  process.env.NODE_OPTIONS = `${previousOptions || ''} --require "${preload}"`;
  process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL = name;
  const channel = new BroadcastChannel(name);
  channel.unref();
  const telemetryRecords = [], traceRecords = [], fallbackRecords = [];
  const pending = new Map();
  let sequence = 0;
  channel.onmessage = ({ data }) => {
    if (data.kind === 'log') { telemetryRecords.push(data.record); onLog?.(data.record); }
    else if (data.kind === 'span') { traceRecords.push(data.record); onSpan?.(data.record); }
    else if (data.kind === 'fallback') fallbackRecords.push(data.record);
    else if (data.kind === 'ack') { pending.get(data.id)?.(); pending.delete(data.id); }
  };
  function command(kind, value) {
    channel.ref();
    return new Promise((resolve, reject) => {
      const id = ++sequence;
      const timeout = setTimeout(() => {
        pending.delete(id);
        channel.unref();
        reject(new Error('message-flow observation marker deadline'));
      }, 5000);
      pending.set(id, () => { clearTimeout(timeout); channel.unref(); resolve(); });
      channel.postMessage({ kind, value, id });
    });
  }
  async function observed() {
    channel.ref();
    const owner = delivery.messageFlowDelivery();
    let startupTimeout;
    try {
      await Promise.race([owner.ready, new Promise((_resolve, reject) => {
        startupTimeout = setTimeout(() => reject(new Error('message-flow bootstrap observation deadline')), 5000);
      })]);
    } finally {
      clearTimeout(startupTimeout);
      channel.unref();
    }
    const deadline = Date.now() + 5000;
    while (owner.executing || Atomics.load(owner.state, delivery.TelemetryState.Pending) !== 0) {
      assert.ok(Date.now() < deadline, 'message-flow provider observation deadline');
      await delay(1);
    }
    // All provider observations precede this marker on the same channel.
    await command('flush');
  }
  return { telemetryRecords, traceRecords, fallbackRecords, observed,
    async reset() {
      await observed();
      await command('fail', false);
      telemetryRecords.length = traceRecords.length = fallbackRecords.length = 0;
    },
    fail() { return command('fail', true); },
    close() {
      channel.close();
      if (previousOptions === undefined) delete process.env.NODE_OPTIONS;
      else process.env.NODE_OPTIONS = previousOptions;
      if (previousChannel === undefined) delete process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL;
      else process.env.ZLINK_TEST_MESSAGE_FLOW_CHANNEL = previousChannel;
    }
  };
};
