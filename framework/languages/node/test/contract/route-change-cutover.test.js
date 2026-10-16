const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

test('cutover retransmits the retained whole batch after a changed ready generation', async () => {
  let generation = 11n;
  let ready = true;
  const sent = [];
  const node = {
    peers: () => [{ routingId: 'target', state: 3 }],
    observePeerRoute: () => ({ generation, ready }),
    sendInfrastructureControl: async (_target, frame) => {
      sent.push(frame);
      return SubmitResult.Ok;
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({ meshNode: () => node });
  const frames = [Buffer.from('relay-a'), Buffer.from('relay-b'), Buffer.from('cutover')];
  const window = {
    meshName: 'mesh',
    targetNodeRid: 'target',
    frames,
    submittedRouteGeneration: 11n
  };
  await runtime.resendSourceCutover(window);
  assert.equal(sent.length, 0);
  generation = 12n;
  ready = false;
  await runtime.resendSourceCutover(window);
  assert.equal(sent.length, 0);
  ready = true;
  await runtime.resendSourceCutover(window);
  assert.deepEqual(sent, frames);
  await runtime.resendSourceCutover(window);
  assert.deepEqual(sent, frames, 'same generation does not resubmit');
});
