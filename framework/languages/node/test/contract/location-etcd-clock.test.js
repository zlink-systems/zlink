const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ClockEngine
} = require('../../packages/framework-locations-etcd/dist/internal/admin-engine');

class Scheduler {
  now = 0n;
  pending = [];
  sequence = 0;

  sleep(milliseconds) {
    return new Promise((resolve) => {
      this.pending.push({
        at: this.now + BigInt(milliseconds),
        sequence: this.sequence++,
        resolve
      });
    });
  }

  async run(work) {
    let settled = false;
    let failure;
    Promise.resolve()
      .then(work)
      .then(
        () => {
          settled = true;
        },
        (error) => {
          failure = error;
          settled = true;
        }
      );
    for (let event = 0; !settled; event++) {
      assert.ok(event < 10000, 'virtual execution must finish');
      await new Promise(setImmediate);
      if (settled) break;
      this.pending.sort((a, b) => (a.at < b.at ? -1 : a.at > b.at ? 1 : a.sequence - b.sequence));
      const next = this.pending.shift();
      assert.ok(next, 'pending operation must have a scheduled response');
      this.now = next.at;
      next.resolve();
    }
    if (failure) throw failure;
    assert.equal(this.pending.length, 0);
  }
}

class ClockModel {
  scheduler = new Scheduler();
  value = 1700000000000n;
  revision = 1n;
  observations = [];
  writes = [];
  conflicts = 0;

  constructor() {
    this.observe();
  }

  observe() {
    this.observations.push({ at: this.scheduler.now, value: this.value });
  }

  process(
    {
      responseDelay = () => 0,
      suspendFirstSleep = 0,
      wall = () => 9000000000000n,
      operationTimeoutMs
    } = {},
    Engine = ClockEngine
  ) {
    let reads = 0;
    let sleeps = 0;
    // With an operation timeout, every call fails once virtual time passes its operation's deadline.
    const expired = (operation) => {
      if (operation !== undefined && this.scheduler.now > operation.deadline)
        throw new Error('DEADLINE_EXCEEDED');
    };
    const store = {
      ...(operationTimeoutMs === undefined
        ? {}
        : { operation: () => ({ deadline: this.scheduler.now + BigInt(operationTimeoutMs) }) }),
      readClock: async (operation) => {
        expired(operation);
        this.observe();
        const snapshot = { value: this.value, revision: this.revision.toString() };
        await this.scheduler.sleep(responseDelay(++reads));
        this.observe();
        return snapshot;
      },
      compareClock: async (revision, value, operation) => {
        await this.scheduler.sleep(0);
        expired(operation);
        this.observe();
        if (revision !== this.revision.toString()) {
          this.conflicts++;
          return false;
        }
        this.value = value;
        this.revision++;
        this.writes.push({ at: this.scheduler.now, value });
        this.observe();
        return true;
      }
    };
    const time = {
      monotonicMs: () => (this.scheduler.now * 2001n) / 2000n,
      wallMs: () => wall(this.scheduler.now),
      sleep: async (milliseconds) => {
        const suspension = sleeps++ === 0 ? suspendFirstSleep : 0;
        await this.scheduler.sleep(BigInt(milliseconds) + BigInt(suspension));
        this.observe();
      }
    };
    return new Engine(store, time);
  }

  async steps(engines, count) {
    await this.scheduler.run(() =>
      Promise.all(
        engines.map(async (engine) => {
          for (let i = 0; i < count; i++) await engine.step();
        })
      )
    );
  }

  checkBounds() {
    for (let i = 0; i < this.observations.length; i++) {
      for (let j = i + 1; j < this.observations.length; j++) {
        const first = this.observations[i];
        const last = this.observations[j];
        const elapsed = last.at - first.at;
        const increase = last.value - first.value;
        assert.ok(increase >= 0n, `Store clock decreased between observations ${i} and ${j}`);
        // 1.01 * 1.0005 = 202101 / 200000; all quantities are integer milliseconds.
        assert.ok(
          increase * 200000n <= 202101n * elapsed + 202000000n,
          `Store clock exceeded bound: interval=${elapsed}ms increase=${increase}ms`
        );
      }
    }
  }
}

test('etcd clock: three processes share revision CAS and respect every observed interval', async () => {
  const model = new ClockModel();
  await model.steps([model.process(), model.process(), model.process()], 12);
  assert.ok(model.writes.length >= 10);
  assert.ok(model.conflicts > 0, 'the test must exercise simultaneous revision comparisons');
  for (let i = 1; i < model.writes.length; i++) {
    assert.ok(model.writes[i].at - model.writes[i - 1].at >= 999n);
  }
  model.checkBounds();
});

test('etcd clock: response delay starts elapsed measurement at the first response', async () => {
  const model = new ClockModel();
  await model.steps(
    [model.process({ responseDelay: (read) => (read === 1 ? 20000 : read === 2 ? 3500 : 17) })],
    5
  );
  assert.equal(model.writes[0].value - 1700000000000n, 1010n);
  model.checkBounds();
});

async function pausedAfterRead(Engine = ClockEngine) {
  const model = new ClockModel();
  await model.steps([model.process({ suspendFirstSleep: 20000 }, Engine)], 5);
  return model;
}

test('etcd clock: a process suspended for 20 seconds after reading adds at most 1010ms', async () => {
  const model = await pausedAfterRead();
  assert.equal(model.writes[0].value - 1700000000000n, 1010n);
  model.checkBounds();
});

test('etcd clock: all processes stopped for 60 seconds do not recover the missing interval at once', async () => {
  const model = new ClockModel();
  await model.steps([model.process(), model.process(), model.process()], 4);
  const beforeRestart = model.value;
  model.scheduler.now += 60000n;
  model.observe();
  const writeCount = model.writes.length;
  await model.steps([model.process(), model.process(), model.process()], 6);
  assert.equal(model.writes[writeCount].value - beforeRestart, 1010n);
  model.checkBounds();
});

test('etcd clock: wall clock changes only affect the bounded catch-up correction', async () => {
  const model = new ClockModel();
  const initial = model.value;
  await model.steps(
    [
      model.process({
        wall: (at) =>
          at < 3000n ? initial + 100000000n : at < 6000n ? 0n : initial + 100000000000n
      })
    ],
    10
  );
  const increments = model.writes.map(
    (write, index) => write.value - (index ? model.writes[index - 1].value : initial)
  );
  assert.ok(
    increments.some((value) => value === 1000n),
    'wall clock retreat must remove catch-up'
  );
  assert.ok(
    increments.some((value) => value === 1010n),
    'wall clock advance must exercise catch-up'
  );
  model.checkBounds();
});

test('etcd clock: catch-up does not advance beyond a wall clock less than 10ms ahead', async () => {
  const model = new ClockModel();
  const initial = model.value;
  await model.steps([model.process({ wall: () => initial + 1005n })], 1);
  assert.equal(model.value - initial, 1005n);
  model.checkBounds();
});

test('etcd clock: an operation timeout shorter than the one-second check still advances the clock', async () => {
  const model = new ClockModel();
  const engine = model.process({ operationTimeoutMs: 900 });
  await model.scheduler.run(async () => {
    for (let i = 0; i < 4; i++) await engine.step();
  });
  assert.ok(
    model.writes.length >= 1,
    'the confirming read and the write must not share the first read deadline'
  );
  model.checkBounds();
});

test('etcd clock: the interval check rejects an engine with the elapsed-time cap removed', async () => {
  class UncappedEngine {
    constructor(store, time) {
      this.store = store;
      this.time = time;
    }

    async step() {
      const first = await this.store.readClock();
      const a = this.time.monotonicMs();
      await this.time.sleep(1000);
      const second = await this.store.readClock();
      if (first.revision !== second.revision) return;
      let elapsed = this.time.monotonicMs() - a;
      const wall = this.time.wallMs();
      if (wall > first.value + elapsed)
        elapsed = elapsed + 10n < wall - first.value ? elapsed + 10n : wall - first.value;
      await this.store.compareClock(first.revision, first.value + elapsed);
    }
  }
  const model = await pausedAfterRead(UncappedEngine);
  assert.ok(model.writes[0].value - 1700000000000n > 1010n);
  assert.throws(() => model.checkBounds(), /Store clock exceeded bound/);
});
