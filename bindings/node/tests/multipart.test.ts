'use strict';

process.env.ZLINK_NODE_TEST_HOOKS = '1';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const zlink = require('@zlink-systems/zlink');

interface HeldMultipartStart {
  state: unknown;
  openResult: number;
  openErrno: number;
}

interface HeldMultipartEnd {
  finalResult: number;
  finalErrno: number;
}

interface SendCloseStressCounts {
  attempts: bigint;
  single_attempts: bigint;
  multipart_attempts: bigint;
  submitted: bigint;
  rejected_einval: bigint;
  shutdown: bigint;
  backpressured: bigint;
  other_submit: bigint;
  received_records: bigint;
  bad_records: bigint;
  bad_first_parts: bigint;
  bad_mixed_parts: bigint;
  bad_part_counts: bigint;
  bad_next_part_results: bigint;
  close_ok: bigint;
  close_busy: bigint;
  close_shutdown: bigint;
  close_other: bigint;
}

const nativeTestHooks = require(
  path.resolve(__dirname, '../../build/Release/zlink.node')
) as {
  testBeginHeldRoutedMultipart(socket: unknown, routingId: Buffer): HeldMultipartStart;
  testEndHeldRoutedMultipart(state: unknown): HeldMultipartEnd;
  testRunSendCloseStress(threadCount: number, iterations: number): SendCloseStressCounts;
};
const { getNativeHandle } = require(
  path.resolve(__dirname, '../../dist/zlink/runtime/handles/native_handle.js')
) as { getNativeHandle(handle: unknown): unknown };

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

test('routed multipart captures its target and preserves part boundaries', async () => {
  const ctx = zlink.createContext();
  const router = zlink.createRouterSocket(ctx);
  const dealer = zlink.createDealerSocket(ctx);
  const inbound = new zlink.Received();
  const outbound = new zlink.Received();
  try {
    router.bind('inproc://node-routed-multipart-contract');
    dealer.connect('inproc://node-routed-multipart-contract');
    await dealer.send().message('route-probe').submit();
    assert.equal(router.recv(inbound), true);
    assert.ok(inbound.routingId);
    const operation = router.send(inbound.routingId)
      .message('first').message('second');
    inbound.close();
    await operation.submit();
    assert.equal(dealer.recv(outbound), true);
    assert.deepEqual(outbound.parts.map((part) => part.getString()), ['first', 'second']);
  } finally {
    outbound.close(); inbound.close(); dealer.close(); router.close(); ctx.close();
  }
});

test('native thread stress mixes single-part, multipart, and close races', () => {
  const counts = nativeTestHooks.testRunSendCloseStress(4, 10_000);
  assert.equal(counts.attempts, 40_000n);
  assert.equal(counts.single_attempts, 20_000n);
  assert.equal(counts.multipart_attempts, 20_000n);
  assert.equal(
    counts.submitted
      + counts.rejected_einval
      + counts.shutdown
      + counts.backpressured
      + counts.other_submit,
    counts.attempts
  );
  assert.ok(counts.submitted > 0n);
  assert.ok(counts.rejected_einval > 0n);
  assert.ok(counts.received_records > 0n);
  assert.equal(counts.bad_records, 0n);
  assert.equal(counts.close_ok, 1n);
  assert.equal(counts.close_shutdown, 0n);
  assert.equal(counts.close_other, 0n);
});
