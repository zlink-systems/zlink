const assert = require('node:assert/strict');
const test = require('node:test');
const deferred = require('./helpers/deferred.cjs');
const fixture = require('../../../../runtime/conformance/login-admission-queue-v1.json');

const key = Buffer.alloc(32, 1).toString('base64url');
const identity = {
  nodeRid: 'bg',
  nodeLifecycleGeneration: '7',
  streamName: 'login',
  sessionRid: 'cw'
};
const owner = { ownerId: 'owner', leaseGeneration: '3' };

for (const { name, given, steps } of fixture.nodeScenarios) {
  test(`login admission node fixture: ${name}`, async () => {
    const {
      AdmissionTickets
    } = require('../../packages/framework/dist/runtime/streams/admission-tickets');
    const {
      AdmissionQueueNode
    } = require('../../packages/framework/dist/runtime/streams/admission-queue-node');
    const codec = new AdmissionTickets(key);
    const now = 1_700_000_000_000;
    const requests = [];
    const releases = [];
    const epoch = String(given.reportedEpoch ?? 42);
    const node = new AdmissionQueueNode(codec, given.globalLimit != null, {
      local: () => identity,
      owner: () => owner,
      limit: () => 8,
      inFlight: () => 0,
      report: async () => ({ epoch, deliveries: [] }),
      acquire: async (request) => {
        requests.push(request);
        if (given.spotPermit === 'noResponseWithinSpotRequestTimeout')
          throw new Error('Spot request timeout');
        return given.spotPermit === 'granted'
          ? { granted: true, permitId: '1' }
          : { granted: false };
      },
      release: (release) => releases.push(release),
      error: () => {},
      now: () => now
    });
    if (given.reportedEpoch != null) await node.report();
    let ticket = codec.issue(
      identity,
      given.ticketState === 'wrong-epoch' ? 41n : BigInt(epoch),
      now + (given.ticketState === 'expired' ? -2001 : 30_000)
    );
    if (given.ticketState === 'missing') ticket = undefined;
    if (given.ticketState === 'bad-signature') {
      const bytes = Buffer.from(ticket, 'base64url');
      bytes[48] ^= 1;
      ticket = bytes.toString('base64url');
    }
    let lease;
    for (const { action, expect } of steps) {
      if (action === 'authenticate') {
        const verified = node.verify(identity, ticket);
        if (expect.decision === 'stageOne') {
          assert.equal(verified, epoch);
          assert.equal(requests.length, 0);
          continue;
        }
        const result =
          verified === undefined ? { kind: 'queueRequired' } : await node.admit(identity, verified);
        assert.equal(
          result.kind,
          expect.errorCode === undefined
            ? 'admitted'
            : expect.metadata === undefined
              ? 'unavailable'
              : 'queueRequired'
        );
        if (result.kind === 'admitted') lease = result.release;
        if (expect.admitPermitRequests !== undefined)
          assert.equal(requests.length, expect.admitPermitRequests);
        if (expect.admitPermitRequestSeq !== undefined)
          assert.equal(requests[0].requestSeq, String(expect.admitPermitRequestSeq));
        if (expect.releasePermitSends !== undefined)
          assert.equal(releases.length, expect.releasePermitSends);
      } else if (action === 'handlerCompleted') {
        lease();
        assert.equal(releases.length, expect.releasePermitSends);
      } else if (action === 'nodeReport') {
        const report = node.snapshot();
        assert.equal(report.resolvedRequestSeq, String(expect.resolvedRequestSeq));
        assert.deepEqual(report.heldPermits, expect.heldPermits);
      } else {
        assert.fail(`Unknown node fixture action: ${action}`);
      }
    }
  });
}

test('a report tick skips an outstanding NodeReport and preserves changes made during it', async () => {
  const {
    AdmissionTickets
  } = require('../../packages/framework/dist/runtime/streams/admission-tickets');
  const {
    AdmissionQueueNode
  } = require('../../packages/framework/dist/runtime/streams/admission-queue-node');
  const reports = [];
  const response = { epoch: '42', deliveries: [] };
  const reply = deferred();
  const node = new AdmissionQueueNode(new AdmissionTickets(key), true, {
    local: () => identity,
    limit: () => 2,
    inFlight: () => 0,
    report: (report) => {
      reports.push(report);
      return reports.length === 1 ? reply.promise : Promise.resolve(response);
    }
  });
  const second = { ...identity, sessionRid: 'dA' };
  node.ticketUsed(identity);
  const tick = () => node.report();
  const firstReport = tick();
  node.ticketUsed(identity);
  node.sessionEnded(second);
  assert.equal(await tick(), undefined);
  assert.equal(reports.length, 1);
  reply.resolve(response);
  await firstReport;
  await tick();
  await tick();
  const used = { streamName: 'login', sessionRid: 'cw' };
  const ended = { streamName: 'login', sessionRid: 'dA' };
  assert.deepEqual(
    reports.map(({ usedTickets, endedSessions }) => ({ usedTickets, endedSessions })),
    [
      { usedTickets: [used], endedSessions: [] },
      { usedTickets: [used], endedSessions: [ended] },
      { usedTickets: [], endedSessions: [] }
    ]
  );
});

test('a failed report or enqueue closes ticket admission until a new report succeeds', async () => {
  const {
    AdmissionTickets
  } = require('../../packages/framework/dist/runtime/streams/admission-tickets');
  const {
    AdmissionQueueNode
  } = require('../../packages/framework/dist/runtime/streams/admission-queue-node');
  const codec = new AdmissionTickets(key);
  const now = 1_700_000_000_000;
  let available = true;
  const reports = [];
  const node = new AdmissionQueueNode(codec, true, {
    local: () => identity,
    owner: () => owner,
    limit: () => 2,
    inFlight: () => 1,
    report: async (report) => {
      reports.push(report);
      if (!available) throw new Error('no Object Server');
      return { epoch: '42', deliveries: [] };
    },
    acquire: async () => ({ granted: true, permitId: '1' }),
    release: () => {},
    now: () => now
  });
  const ticket = codec.issue(identity, 42n, now + 30_000);
  await node.report();
  assert.equal(node.verify(identity, ticket), '42');
  const { release } = await node.admit(identity, '42');

  node.ticketUsed(identity);
  node.sessionEnded({ ...identity, sessionRid: 'dA' });
  available = false;
  await assert.rejects(node.report(), /no Object Server/);
  assert.equal(node.verify(identity, ticket), undefined);
  assert.deepEqual(node.snapshot().heldPermits, ['1']);

  available = true;
  await node.report();
  assert.deepEqual(reports[1].usedTickets, [{ streamName: 'login', sessionRid: 'cw' }]);
  assert.deepEqual(reports[1].endedSessions, [{ streamName: 'login', sessionRid: 'dA' }]);
  assert.deepEqual(reports[2].usedTickets, reports[1].usedTickets);
  assert.deepEqual(reports[2].endedSessions, reports[1].endedSessions);
  assert.equal(node.verify(identity, ticket), '42');
  assert.deepEqual(node.snapshot().heldPermits, ['1']);
  release();

  await assert.rejects(
    node.enqueue(async () => {
      throw new Error('no Object Server');
    }),
    /no Object Server/
  );
  assert.equal(node.verify(identity, ticket), undefined);
});
