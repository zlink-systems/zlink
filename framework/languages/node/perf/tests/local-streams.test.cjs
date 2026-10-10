const assert = require('node:assert/strict');
const test = require('node:test');

const scenarios = [
  ['spot-no-await-echo', 'SpotNoAwaitEchoScenario'],
  ['spot-worker-offload-echo', 'SpotWorkerOffloadEchoScenario'],
  ['s2s-spot-to-channel-request-echo', 'S2sSpotToChannelRequestEchoScenario'],
  ['s2s-spot-to-channel-send-send-echo', 'S2sSpotToChannelSendSendEchoScenario']
];
const turn = () => new Promise((resolve) => setImmediate(resolve));

for (const [file, name] of scenarios) {
  test(`${file}: each local stream waits for its own terminal`, async () => {
    const Scenario = require(`../build/spot-server/${file}-scenario.js`)[name];
    let open = true;
    const errors = [];
    const measurement = {
      get canIssue() {
        return open;
      },
      recordDiagnostic: (e) => errors.push(e)
    };
    const config = { workload: { logicalStreams: 2 } };
    const scenario = new Scenario(null, null, measurement, config, null, null, null, null);
    const calls = [[], []];
    // Hold the operation terminal independently on each stream.
    scenario.loop = (stream) => new Promise((resolve) => calls[stream].push(resolve));
    const running = scenario.run();
    await turn();
    const before = calls.map((pending) => pending.length);
    calls[0][0]();
    await turn();
    const after = calls.map((pending) => pending.length);
    open = false;
    for (const pending of calls) for (const resolve of pending) resolve();
    await running;
    assert.deepEqual(before, [1, 1]);
    assert.deepEqual(after, [2, 1]);
    assert.deepEqual(errors, []);
  });
}

test('local send/send driver terminal does not wait for the remote echo', async () => {
  const {
    S2sSpotToChannelSendSendEchoScenario: Scenario
  } = require('../build/spot-server/s2s-spot-to-channel-send-send-echo-scenario.js');
  let release;
  const echo = new Promise((resolve) => {
    release = resolve;
  });
  let completed = 0;
  const errors = [];
  const request = {
    correlationId: 'echo',
    with() {
      return this;
    }
  };
  const measurement = {
    canIssue: true,
    request: () => request,
    callTimeout: () => 1000,
    recordDiagnostic: (error) => errors.push(error),
    completeOperation: () => {
      completed++;
      return true;
    }
  };
  const spots = {
    requestToSpot: () => ({
      timeout() {
        return this;
      },
      submit: async () => ({ started: true })
    })
  };
  const correlations = { find: () => ({ startedTicks: 0n }), completeAsync: () => echo };
  const metrics = { count() {}, record() {} };
  const scenario = new Scenario(
    spots,
    null,
    measurement,
    { spotIds: ['spot'] },
    null,
    null,
    metrics,
    correlations
  );
  scenario.sequences = [0];
  let driverEnded = false;
  const running = scenario.loop(0).then(() => {
    driverEnded = true;
  });
  await turn();
  const endedBeforeEcho = driverEnded;
  const completedBeforeEcho = completed;
  release({ error: undefined, completedTicks: 1n });
  await running;
  await turn();
  assert.equal(endedBeforeEcho, true);
  assert.equal(completedBeforeEcho, 0);
  assert.equal(completed, 1);
  assert.deepEqual(errors, []);
});
