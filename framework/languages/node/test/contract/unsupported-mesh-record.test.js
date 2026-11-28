const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const backend = require('../../packages/framework/dist/runtime/backend');

const owner = { ownerKind: framework.ReadyOwnerKind.Node };
const record = { kind: 13, parts: [] };

test('the backend dispatcher rejects reserved receive kind 13', () => {
  assert.equal(framework.ReceiveKind.TransferControl, 13);
  const dispatcher = new backend.ZLinkMeshRecordDispatcher({});
  assert.throws(
    () =>
      framework.runZLinkExecutionArea('infrastructure', () => dispatcher.dispatch(owner, record)),
    /Unsupported MeshNode receive kind '13'/
  );
});

test('the host dispatcher rejects reserved receive kind 13', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({})
  });
  assert.throws(
    () => host.dispatchMeshRecord(() => 'remote', 'game', owner, record),
    /MeshNode record kind '13' does not yet have a registered framework consumer/
  );
});
