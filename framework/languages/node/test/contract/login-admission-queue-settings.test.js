'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const {
  toStreamNodeMap
} = require('../../packages/framework/dist/contracts/Configuration/RegistrationNormalizers');
const {
  createFrameworkOptions
} = require('../../packages/framework/dist/contracts/Configuration/RegistrationBuilders');
const { zlinkFramework } = require('../../packages/nestjs/dist');

const key = Buffer.alloc(32, 1).toString('base64url');
const previousKey = Buffer.alloc(64, 2).toString('base64url');
const enabled = {
  authenticationPacketName: 'Login',
  admissionQueueEnabled: true,
  admissionQueueKey: key
};
const network = { bindHost: '127.0.0.1' };

function options(values) {
  return toStreamNodeMap(values, network);
}

test('the Framework and NestJS STREAM builders carry the five queue settings', () => {
  class Session {}
  const configure = (builder) => {
    builder
      .addStreamNode('login')
      .bind()
      .registerSession(Session)
      .setAuthenticationAdmission('Login')
      .setAdmissionQueueEnabled(true)
      .setAdmissionQueueKey(key)
      .setAdmissionQueuePreviousKey(previousKey)
      .setAdmissionQueueMeshName('login-mesh')
      .setAdmissionGlobalLimit(2147483647);
  };
  const nest = zlinkFramework();
  configure(nest);
  for (const { login } of [createFrameworkOptions(configure).streamNodes, nest.build().streams]) {
    assert.equal(login.admissionQueueEnabled, true);
    assert.equal(login.admissionQueueKey, key);
    assert.equal(login.admissionQueuePreviousKey, previousKey);
    assert.equal(login.admissionQueueMeshName, 'login-mesh');
    assert.equal(login.admissionGlobalLimit, 2147483647);
  }
});

test('disabled queue settings are ignored and default to disabled', () => {
  const result = options({
    a: {},
    b: {
      admissionQueueEnabled: false,
      admissionQueueKey: 'invalid',
      admissionQueuePreviousKey: 'invalid',
      admissionQueueMeshName: 'invalid',
      admissionGlobalLimit: 0
    }
  });
  for (const listener of result.values()) {
    assert.equal(listener.admissionQueueEnabled, false);
    assert.equal(listener.admissionQueueKey, undefined);
    assert.equal(listener.admissionQueuePreviousKey, undefined);
    assert.equal(listener.admissionQueueMeshName, undefined);
    assert.equal(listener.admissionGlobalLimit, undefined);
  }
});

test('enabled listeners require matching queue settings', () => {
  const settings = { ...enabled, admissionQueuePreviousKey: previousKey, admissionGlobalLimit: 1 };
  assert.deepEqual(
    options({ a: settings, b: settings }).get('b'),
    options({ b: settings }).get('b')
  );
  for (const different of [
    { admissionQueueEnabled: false },
    { admissionQueueKey: previousKey },
    { admissionQueuePreviousKey: key },
    { admissionQueueMeshName: 'other-mesh' },
    { admissionGlobalLimit: 2 }
  ]) {
    assert.throws(() => options({ a: settings, b: { ...settings, ...different } }), /must match/);
  }
});

test('invalid queue configuration raises a startup configuration error without disclosing keys', () => {
  for (const [invalid, field] of [
    [{ admissionQueueEnabled: 'true' }, 'admissionQueueEnabled'],
    [{ authenticationPacketName: undefined }, 'authenticationPacketName'],
    [{ admissionQueueKey: undefined }, 'admissionQueueKey'],
    [{ admissionQueueKey: 'secret!' }, 'admissionQueueKey'],
    [{ admissionQueueKey: key + '=' }, 'admissionQueueKey'],
    [{ admissionQueuePreviousKey: 'secret!' }, 'admissionQueuePreviousKey'],
    ...[0, 1.5, 2147483648, '1'].map((value) => [
      { admissionGlobalLimit: value },
      'admissionGlobalLimit'
    ])
  ]) {
    assert.throws(
      () => options({ a: { ...enabled, ...invalid } }),
      (error) => {
        assert.equal(error.name, 'ZLinkConfigurationException');
        assert.ok(error.message.includes(field));
        assert.ok(!error.message.includes('secret!'));
        assert.ok(!error.message.includes(key));
        return true;
      }
    );
  }
});

const fixture = require('../../../../runtime/conformance/login-admission-queue-v1.json');

test('login admission fixture has 7 startup scenarios', () => {
  assert.equal(fixture.startupScenarios.length, 7);
});

for (const { name, given, expect } of fixture.startupScenarios) {
  test(`login admission queue startup fixture: ${name}`, () => {
    const {
      createFrameworkRegistration
    } = require('../../packages/framework/dist/contracts/Configuration/Registration');
    const size = given.streamNameUtf8Bytes;
    const streamName =
      size === undefined ? 'login' : '한'.repeat(Math.floor(size / 3)) + 'a'.repeat(size % 3);
    const select = () =>
      createFrameworkRegistration({
        streamNodes: {
          [streamName]: {
            bind: 'tcp://127.0.0.1:0',
            session: class Session {},
            authenticationPacketName: 'Login',
            admissionQueueEnabled: given.queueEnabled,
            admissionQueueKey: key,
            admissionQueueMeshName: given.queueMeshName ?? undefined
          }
        },
        spotNodes: Object.fromEntries(
          given.meshNodes.map((mesh) => [mesh, { router: { bind: 'tcp://127.0.0.1:0' } }])
        )
      }).admissionQueueRoute;
    if (!expect.started) {
      assert.throws(select, {
        name: 'ZLinkConfigurationException',
        ...(typeof expect.configurationError === 'string'
          ? { message: new RegExp(expect.configurationError) }
          : {})
      });
    } else {
      const selection = select();
      assert.equal(selection?.meshName ?? null, expect.queueMesh);
      assert.equal(selection?.spotId, expect.queueSpotId);
    }
  });
}

test('an enabled admission queue rejects an empty STREAM registration name', () => {
  assert.throws(() => options({ '': enabled }), { name: 'ZLinkConfigurationException' });
});
