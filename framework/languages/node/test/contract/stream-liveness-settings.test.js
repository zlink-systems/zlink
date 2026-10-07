const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

class Session {}
const endpoint = 'tcp://127.0.0.1:0';

function registration(configure) {
  return framework.createFrameworkRegistrationWithBuilder((options) => {
    configure(options.addStreamNode('stream').bind(endpoint).registerSession(Session));
  });
}

test('STREAM builder defaults and custom liveness settings survive normalization', () => {
  const defaults = registration(() => {}).streamNodes.get('stream');
  assert.equal(defaults.heartbeatIntervalMs, 1000);
  assert.equal(defaults.heartbeatTimeoutMs, 5000);
  assert.equal(defaults.idleTimeoutMs, 0);
  const configured = registration((builder) => builder.setHeartbeat(2000, 6000).setIdleTimeout(3000)).streamNodes.get('stream');
  assert.equal(configured.heartbeatIntervalMs, 2000);
  assert.equal(configured.heartbeatTimeoutMs, 6000);
  assert.equal(configured.idleTimeoutMs, 3000);
});

for (const [interval, timeout, message] of [
  [0, 5000, 'STREAM heartbeat interval must be positive.'],
  [-1, 5000, 'STREAM heartbeat interval must be positive.'],
  [NaN, 5000, 'STREAM heartbeat interval must be positive.'],
  [Infinity, 5000, 'STREAM heartbeat interval must be positive.'],
  [1000, 0, 'STREAM heartbeat timeout must be positive.'],
  [1000, -1, 'STREAM heartbeat timeout must be positive.'],
  [1000, Infinity, 'STREAM heartbeat timeout must be positive.'],
  [1000, 1000, 'STREAM heartbeat timeout must be greater than interval.'],
  [1000, 999, 'STREAM heartbeat timeout must be greater than interval.']
]) {
  test(`invalid STREAM heartbeat ${interval}/${timeout} is rejected before start`, () => {
    assert.throws(() => registration((builder) => builder.setHeartbeat(interval, timeout)), { name: 'ZLinkConfigurationException', message });
  });
}

for (const value of [-1, NaN, Infinity]) {
  test(`invalid STREAM idle ${value} is rejected before start`, () => {
    assert.throws(() => registration((builder) => builder.setIdleTimeout(value)), { name: 'ZLinkConfigurationException', message: 'STREAM idle timeout must not be negative.' });
  });
}

test('Nest STREAM builder forwards custom settings to the Framework validator', () => {
  const built = nestjs.zlinkFramework().addStreamNode('stream').bind(endpoint).setHeartbeat(2000, 6000).setIdleTimeout(3000).registerSession(Session).build();
  const configured = framework.createFrameworkRegistration({ streamNodes: built.streams }).streamNodes.get('stream');
  assert.equal(configured.heartbeatIntervalMs, 2000);
  assert.equal(configured.heartbeatTimeoutMs, 6000);
  assert.equal(configured.idleTimeoutMs, 3000);
  const invalid = nestjs.zlinkFramework().addStreamNode('stream').bind(endpoint).setHeartbeat(1000, 1000).registerSession(Session).build();
  assert.throws(() => framework.createFrameworkRegistration({ streamNodes: invalid.streams }), { name: 'ZLinkConfigurationException', message: 'STREAM heartbeat timeout must be greater than interval.' });
});
