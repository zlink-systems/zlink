const assert = require('node:assert/strict');
const test = require('node:test');
const fixture = require('../../../../runtime/conformance/login-admission-queue-v1.json');
const {
  AdmissionQueueSpot,
  admissionQueueInstanceFactory,
  estimateWaitMs
} = require('../../packages/framework/dist/runtime/streams/admission-queue-spot');
const {
  AdmissionTickets
} = require('../../packages/framework/dist/runtime/streams/admission-tickets');
const b64 = (value) => Buffer.from(value).toString('base64url');
const ownerFor = (value = {}) => ({
  ownerId: value.ownerId ?? 'owner',
  leaseGeneration: String(value.leaseGeneration ?? 1)
});
const identity = (session, value = {}) => ({
  nodeRid: b64(value.node ?? value.nodeRid ?? 'n'),
  nodeLifecycleGeneration: String(value.nodeLifecycleGeneration ?? 1),
  streamName: value.streamName ?? 'login',
  sessionRid: b64(session)
});
const local = (value) => {
  const { nodeRid, nodeLifecycleGeneration } = identity('', value);
  return { nodeRid, nodeLifecycleGeneration };
};
const sessionIdentity = ({ streamName, sessionRid }) => ({ streamName, sessionRid });
const text = (value) => Buffer.from(value, 'base64url').toString();
const summary = (value) => ({ node: text(value.nodeRid), sessionRid: text(value.sessionRid) });

function configureQueueSpot(codec, globalLimit, isOwnerLive) {
  const handlers = new Map();
  const context = {
    handlers: {
      addPacket(handler) {
        handlers.set(handler.name, new handler());
        return this;
      },
      addTimerHandler() {}
    },
    async addTimer(_name, _interval, handler) {
      handlers.set('maintenance', new handler());
    },
    runIoWorker(work) {
      return { yield: () => work() };
    }
  };
  const Spot = admissionQueueInstanceFactory(codec, globalLimit, isOwnerLive);
  const spot = new Spot(context);
  spot.configure();
  return {
    spot,
    handle: (name, request) => handlers.get(name).handle(spot, request)
  };
}

test('login admission fixture has 22 Spot scenarios', () => {
  assert.equal(fixture.queueScenarios.length, 22);
});

for (const { name, given, steps } of fixture.queueScenarios) {
  test(`login admission Spot fixture: ${name}`, async () => {
    const codec = new AdmissionTickets(Buffer.alloc(32, 1).toString('base64url'));
    let now = 10_000;
    const epoch = BigInt(given.epoch ?? 42);
    const permits = new Map();
    let last;
    const permitRequest = (step) => ({
      ...identity(step.sessionRid ?? 's1', step),
      ...ownerFor(step),
      epoch: String(step.epoch ?? epoch),
      requestSeq: String(step.requestSeq ?? 1)
    });
    // A fixture starts from a snapshot, which can include tickets in excess of today's budget.
    const running =
      typeof given.runningPermits === 'number'
        ? Array.from({ length: given.runningPermits }, () => ({}))
        : (given.runningPermits ?? []);
    const initialPermits = new Map(
      running.map((entry, index) => {
        const permitId = String(index + 1);
        last = { ...local(entry), permitId };
        permits.set(entry.permit, permitId);
        return [permitId, permitRequest(entry)];
      })
    );
    const waiters = (given.waiting ?? given.waiters ?? []).map((entry) => {
      const connection =
        typeof entry === 'string' ? identity(entry) : identity(entry.sessionRid, entry);
      return { connection, hasTicket: entry.ticket === true };
    });
    const initialWaiters =
      given.reports === undefined && given.reportAgeMs === undefined
        ? waiters.map(({ connection, hasTicket }) =>
            hasTicket ? issuedWaiter(connection) : { connection }
          )
        : [];
    for (const entry of given.validTickets ?? [])
      initialWaiters.push(issuedWaiter(identity(entry.sessionRid, entry)));

    function issuedWaiter(connection) {
      const expiresAt = now + 30_000;
      return {
        connection,
        ticket: { ticket: codec.issue(connection, epoch, expiresAt), expiresInMs: 30_000 },
        expiresAt,
        ticketDelivery: 'sent'
      };
    }

    const ownerLeaseChecks = [];
    const configured = configureQueueSpot(codec, given.globalLimit, async (owner) => {
      ownerLeaseChecks.push(owner);
      return given.ownerLeases?.[owner.ownerId + ':' + owner.leaseGeneration] !== 'ended';
    });
    let spot = new AdmissionQueueSpot(
      codec,
      given.globalLimit,
      epoch,
      () => now,
      () => now,
      { waiters: initialWaiters, permits: initialPermits }
    );
    const report = (step) => {
      const previous = given.reports?.find((entry) => entry.node === step.node);
      configured.spot.queue = spot;
      return configured.handle('NodeReportHandler', {
        ...local(step),
        limit: step.limit ?? previous?.limit ?? 0,
        inFlight: step.inFlight ?? previous?.inFlight ?? 0,
        usedTickets: (step.usedTickets ?? step.usedTicketSessions ?? []).map((entry) =>
          sessionIdentity(identity(entry.sessionRid ?? entry, { ...step, ...entry }))
        ),
        endedSessions: (step.endedSessions ?? []).map((session) =>
          sessionIdentity(identity(session, step))
        ),
        heldPermits: (step.heldPermits ?? []).map((id) => permits.get(id)),
        resolvedRequestSeq: String(step.resolvedRequestSeq ?? 0)
      });
    };
    const admit = (step) => {
      const request = permitRequest(step);
      const result = spot.admit(request);
      if (result.permitId !== undefined) {
        last = { ...local(step), permitId: result.permitId };
        permits.set(step.permit, result.permitId);
      }
      return result;
    };
    if (given.position !== undefined) {
      for (let i = 1; i < given.position; i++)
        spot.enqueue(identity('other' + i, { node: 'other' }));
      spot.enqueue(identity(given.sessionRid, given));
    }
    for (const entry of given.reports ?? []) {
      await report(entry);
      for (let i = 0; i < (entry.issuedSinceReport ?? 0); i++)
        spot.enqueue(identity('issued' + i, entry));
    }
    if (given.reportAgeMs !== undefined) {
      await report({ limit: 1 });
      // The waiter must first arrive after the report is stale.
      now += given.reportAgeMs;
      spot = new AdmissionQueueSpot(
        codec,
        undefined,
        42n,
        () => now,
        () => now
      );
      now -= given.reportAgeMs;
      await report({ limit: 1 });
      now += given.reportAgeMs;
      for (const { connection } of waiters) spot.enqueue(connection);
    }
    if (given.reports !== undefined && !steps.some(({ action }) => action === 'issue'))
      for (const { connection } of waiters) spot.enqueue(connection);
    let ticket;
    for (const step of steps) {
      const { action, expect } = step;
      switch (action) {
        case 'enqueue': {
          const before = spot.waiters().length;
          const { delivery: result } = spot.enqueue(
            identity(step.sessionRid ?? given.sessionRid, { ...given, ...step })
          );
          assert.equal(spot.waiters().length - before, expect.newWaiters);
          if (expect.position !== undefined) assert.equal(result.position, expect.position);
          if (expect.ticket === null) assert.equal(result.ticket, undefined);
          else if (expect.ticket !== undefined) assert.equal(result.ticket, ticket);
          break;
        }
        case 'issueTicket': {
          await report({ ...given, limit: 1 });
          ticket = spot.enqueue(identity(given.sessionRid, given)).delivery.ticket;
          assert.equal(expect.ticket, 'T');
          assert.ok(codec.verify(ticket, identity(given.sessionRid, given), 42n, now));
          break;
        }
        case 'calculateBudget':
          assert.equal(spot.budget(local({ node: 'n' })), expect.budget);
          break;
        case 'issue': {
          if (given.reports !== undefined)
            for (const { connection } of waiters) spot.enqueue(connection);
          for (const entry of given.invalidatedTickets ?? []) await report({ ...entry, limit: 1 });
          spot.advance();
          const status = spot
            .waiters()
            .filter((entry) => !text(entry.connection.sessionRid).startsWith('issued'));
          assert.deepEqual(
            status
              .filter((entry) => entry.ticket !== undefined)
              .map((entry) => text(entry.connection.sessionRid)),
            expect.ticketSessions
          );
          if (expect.remaining !== undefined)
            assert.deepEqual(
              status
                .filter((entry) => entry.ticket === undefined)
                .map((entry) => text(entry.connection.sessionRid)),
              expect.remaining
            );
          break;
        }
        case 'admitPermit': {
          const result = admit(step);
          assert.equal(result.granted, expect.granted);
          if (expect.permitIdNonzero) assert.notEqual(BigInt(result.permitId), 0n);
          break;
        }
        case 'releasePermit':
          spot.release(last);
          break;
        case 'nodeWaiters':
          assert.deepEqual(
            spot
              .waiters()
              .filter(
                ({ connection, ticket }) =>
                  connection.nodeRid === local(step).nodeRid && ticket === undefined
              )
              .map(({ connection }) => text(connection.sessionRid)),
            expect.order
          );
          break;
        case 'ownerLeaseEnded':
          spot.ownerLeaseEnded(ownerFor(step));
          break;
        case 'nodeReport': {
          const before = ownerLeaseChecks.length;
          const { deliveries } = await report(step);
          if (expect.ownerLeaseChecks !== undefined)
            assert.equal(ownerLeaseChecks.length - before, expect.ownerLeaseChecks);
          if (expect.ticketSessions !== undefined)
            assert.deepEqual(
              deliveries
                .filter(({ kind }) => kind === 'ticket')
                .map(({ sessionRid }) => text(sessionRid)),
              expect.ticketSessions
            );
          if (expect.waiting !== undefined)
            assert.deepEqual(
              spot.waiters().map((entry) => summary(entry.connection)),
              expect.waiting
            );
          if (expect.waiters !== undefined)
            assert.deepEqual(
              spot.waiters().map((entry) => text(entry.connection.sessionRid)),
              expect.waiters
            );
          if (name === 'delete-waiters' && step.endedSessions !== undefined)
            await report({ limit: 1 });
          break;
        }
        case 'ticketExpires':
          now += 30_001;
          spot.advance();
          assert.deepEqual(spot.waiters(), expect.waiters);
          break;
        case 'estimate':
          assert.equal(
            estimateWaitMs(step.position, step.ticketsLast30Seconds),
            expect.estimatedWaitMs
          );
          break;
        case 'reinitialize':
          ticket = codec.issue(identity('a'), BigInt(given.epoch), now + 30_000);
          spot = new AdmissionQueueSpot(
            codec,
            undefined,
            BigInt(expect.epoch),
            () => now,
            () => now
          );
          assert.equal(spot.epoch, String(expect.epoch));
          assert.deepEqual(spot.waiters(), expect.waiting);
          break;
        case 'verifyOldTicket':
          assert.equal(codec.verify(ticket, identity('a'), BigInt(spot.epoch), now), expect.accept);
          break;
        default:
          assert.fail(`Unknown queue fixture action: ${action}`);
      }
      if (expect.runningPermits !== undefined)
        assert.equal(spot.runningPermits, expect.runningPermits);
    }
  });
}

test('reports carry position changes, five-second updates and tickets for the reporting node', async () => {
  const codec = new AdmissionTickets(Buffer.alloc(32, 1).toString('base64url'));
  const a = identity('a', { node: 'n1' });
  const b = identity('b', { node: 'n2' });
  let now = 0;
  const spot = new AdmissionQueueSpot(
    codec,
    undefined,
    42n,
    () => now,
    () => now
  );
  const position = (connection, position, estimatedWaitMs) => ({
    ...sessionIdentity(connection),
    kind: 'position',
    position,
    estimatedWaitMs
  });
  const report = async (connection, limit = 0) =>
    (
      await spot.report(
        {
          ...connection,
          limit,
          inFlight: 0,
          usedTickets: [],
          endedSessions: [],
          heldPermits: [],
          resolvedRequestSeq: '0'
        },
        unexpectedLeaseCheck
      )
    ).deliveries;
  assert.deepEqual(spot.enqueue(a), { epoch: '42', delivery: position(a, 1, 1000) });
  assert.deepEqual(spot.enqueue(b), { epoch: '42', delivery: position(b, 2, 2000) });
  now = 4999;
  assert.deepEqual(await report(a), []);
  assert.deepEqual(await report(b), []);
  now = 5000;
  assert.deepEqual(await report(a), [position(a, 1, 1000)]);
  assert.deepEqual(await report(b), [position(b, 2, 2000)]);
  const [issued] = await report(a, 1);
  assert.equal(issued.kind, 'ticket');
  assert.equal(issued.sessionRid, a.sessionRid);
  assert.ok(codec.verify(issued.ticket, a, 42n, now));
  assert.deepEqual(await report(b), [position(b, 1, 30_000)]);
  assert.deepEqual(await report(a), []);
  assert.deepEqual(await report(b), []);
});

test('an expired ticket cannot grant a permit before Spot maintenance runs', async () => {
  const codec = new AdmissionTickets(Buffer.alloc(32, 1).toString('base64url'));
  const connection = identity('a');
  let utcNow = 10_000;
  const spot = new AdmissionQueueSpot(
    codec,
    1,
    42n,
    () => 0,
    () => utcNow
  );
  await spot.report(
    {
      nodeRid: connection.nodeRid,
      nodeLifecycleGeneration: connection.nodeLifecycleGeneration,
      limit: 1,
      inFlight: 0,
      usedTickets: [],
      endedSessions: [],
      heldPermits: [],
      resolvedRequestSeq: '0'
    },
    unexpectedLeaseCheck
  );
  const { delivery } = spot.enqueue(connection);
  assert.equal(delivery.kind, 'ticket');
  utcNow += delivery.expiresInMs;
  assert.ok(codec.verify(delivery.ticket, connection, 42n, utcNow));
  assert.deepEqual(spot.admit({ ...connection, ...ownerFor(), epoch: '42', requestSeq: '1' }), {
    granted: false
  });
  assert.equal(spot.runningPermits, 0);
});

function unexpectedLeaseCheck() {
  assert.fail('No lease lookup is expected');
}

async function createLeaseCheckScenario(readLease = async (owner) => owner.ownerId !== 'ended') {
  const checks = [];
  const codec = new AdmissionTickets(Buffer.alloc(32, 1).toString('base64url'));
  const { spot, handle } = configureQueueSpot(codec, 2, async (owner) => {
    checks.push(owner);
    return readLease(owner);
  });
  const permitRequest = (session, ownerId) => ({
    ...identity(session),
    ...ownerFor({ ownerId }),
    epoch: spot.queue.epoch,
    requestSeq: '1'
  });
  const ended = permitRequest('a', 'ended');
  const live = permitRequest('b', 'live');
  const report = (node = {}, limit = 2, inFlight = 0, usedTickets = []) =>
    handle('NodeReportHandler', {
      ...local(node),
      limit,
      inFlight,
      usedTickets: usedTickets.map((session) => sessionIdentity(identity(session, node))),
      endedSessions: [],
      heldPermits: [],
      resolvedRequestSeq: '0'
    });
  await report();
  assert.equal(spot.queue.enqueue(ended).delivery.kind, 'ticket');
  assert.equal(spot.queue.enqueue(live).delivery.kind, 'ticket');
  assert.equal(spot.queue.admit(ended).granted, true);
  return {
    checks,
    spot,
    live,
    admit: (request) => handle('AdmitPermitHandler', request),
    report,
    maintain: async () => {
      await spot.onInitialize();
      await handle('maintenance');
    }
  };
}

test('lease reads occur neither on the maintenance tick nor before a permit reaches the cap', async () => {
  const { checks, spot, live, admit, maintain } = await createLeaseCheckScenario();
  await maintain();
  assert.equal(checks.length, 0);
  assert.equal(spot.queue.runningPermits, 1);
  assert.equal((await admit(live)).granted, true);
  assert.equal(checks.length, 0);
  await maintain();
  assert.equal(checks.length, 0);
});

test('AdmitPermit at the cap reclaims ended leases before deciding the grant', async () => {
  const { checks, spot, live, admit } = await createLeaseCheckScenario();
  assert.equal(spot.queue.admit(live).granted, true);
  assert.equal((await admit({ ...live, requestSeq: '2' })).granted, true);
  assert.deepEqual(checks, [
    { ownerId: 'ended', leaseGeneration: 1n },
    { ownerId: 'live', leaseGeneration: 1n }
  ]);
  assert.equal(spot.queue.runningPermits, 2);
});

test('reports skip lease reads when no locally eligible waiter is blocked', async () => {
  const { checks, spot, live, admit, report } = await createLeaseCheckScenario();
  const other = { node: 'other' };
  const first = identity('c', other);
  const second = identity('d', other);

  await report(other);
  assert.equal(checks.length, 0);
  assert.equal((await admit(live)).granted, true);
  await report({}, 2, 0, ['a', 'b']);
  assert.equal(checks.length, 0);
  assert.equal(spot.queue.enqueue(first).delivery.kind, 'position');
  assert.equal(spot.queue.enqueue(second).delivery.kind, 'position');
  await report(other, 2, 2);
  assert.equal(checks.length, 0);
  assert.equal(spot.queue.runningPermits, 2);

  const { deliveries } = await report(other);
  assert.deepEqual(checks, [
    { ownerId: 'ended', leaseGeneration: 1n },
    { ownerId: 'live', leaseGeneration: 1n }
  ]);
  assert.equal(spot.queue.runningPermits, 1);
  assert.deepEqual(
    deliveries.filter(({ kind }) => kind === 'ticket').map(({ sessionRid }) => sessionRid),
    [first.sessionRid]
  );
});

test('a used ticket leaves the queue and budget but can still admit until expiry', async () => {
  const codec = new AdmissionTickets(Buffer.alloc(32, 1).toString('base64url'));
  let now = 10_000;
  const spot = new AdmissionQueueSpot(
    codec,
    1,
    42n,
    () => now,
    () => now
  );
  const first = identity('first');
  const second = identity('second');
  const third = identity('third');
  const permit = (connection, requestSeq) => ({
    ...connection,
    ...ownerFor(),
    epoch: '42',
    requestSeq
  });
  const report = (usedTickets) =>
    spot.report(
      {
        ...local({}),
        limit: 3,
        inFlight: 0,
        usedTickets,
        endedSessions: [],
        heldPermits: [],
        resolvedRequestSeq: '0'
      },
      unexpectedLeaseCheck
    );
  await report([]);
  assert.equal(spot.enqueue(first).delivery.kind, 'ticket');
  assert.equal(spot.enqueue(second).delivery.position, 1);
  assert.equal(spot.enqueue(third).delivery.position, 2);
  const original = spot.admit(permit(first, '1'));
  assert.equal(original.granted, true);
  spot.release({ ...local({}), permitId: original.permitId });

  const { deliveries } = await report([sessionIdentity(first)]);
  assert.deepEqual(
    spot.waiters().map(({ connection }) => connection),
    [second, third]
  );
  assert.equal(deliveries[0].kind, 'ticket');
  assert.equal(deliveries[0].sessionRid, second.sessionRid);
  assert.equal(spot.enqueue(third).delivery.position, 1);
  assert.equal(spot.admit(permit({ ...first, nodeLifecycleGeneration: '2' }, '2')).granted, false);
  const retry = spot.admit(permit(first, '2'));
  assert.equal(retry.granted, true);
  spot.release({ ...local({}), permitId: retry.permitId });
  now += 30_001;
  assert.equal(spot.admit(permit(first, '3')).granted, false);
});

test('lease read failures reject the capped permit and report requests', async () => {
  const failure = new Error('lease read failed');
  let fail = true;
  const { spot, live, admit, report } = await createLeaseCheckScenario(async (owner) => {
    if (fail) throw failure;
    return owner.ownerId !== 'ended';
  });
  assert.equal((await admit(live)).granted, true);
  const retry = { ...live, requestSeq: '2' };
  await assert.rejects(admit(retry), (error) => error === failure);
  assert.equal(spot.queue.runningPermits, 2);
  const other = { node: 'other' };
  const waiting = identity('waiting', other);
  assert.equal(spot.queue.enqueue(waiting).delivery.kind, 'position');
  await assert.rejects(report(other), (error) => error === failure);
  assert.equal(spot.queue.runningPermits, 2);
  fail = false;
  assert.equal((await admit(retry)).granted, true);
});
