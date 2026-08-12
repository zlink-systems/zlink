const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../../e2e/ResilienceLifecycle');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');
const pubSubRoot = path.resolve(__dirname, '../../e2e/PubSub');
const readPubSub = (relative) => fs.readFileSync(path.join(pubSubRoot, relative), 'utf8');

test('PS-B1 keeps the implemented slow-subscriber isolation and records the deferred scale profile', () => {
  const scenario = readPubSub('Client/Scenarios/ps-b1-slow-subscriber-scenario.ts');
  const featureMap = readPubSub('feature-map.ko.md');

  assert.match(scenario, /fastSubscribers\.map/);
  assert.match(scenario, /publishEvent\(/);
  assert.match(scenario, /['"]seq=8['"]/);
  assert.match(scenario, /delay-start\|/);
  assert.match(featureMap, /\| `PS-B1` \| 부분 구현 \|/);
  assert.match(featureMap, /통합된 scale profile은 E2E 구현 단계에서 같은 scenario에 추가한다/);
});

test('RL-D4 channel replies preserve the canonical Error and Response wire headers', () => {
  const envelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
  const request = {
    formatMarker: 0xf2,
    kind: 1,
    channelName: 'profile',
    messageName: 'MissingProfileReq',
    contentType: 'application/json',
    correlationId: 'rl-d4-correlation',
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  };

  const errorHeader = JSON.parse(Buffer.from(
    envelope.encodeChannelErrorReplyParts(request, new Error('missing handler'))[0]
  ).toString('utf8'));
  assert.equal(errorHeader.kind, 5);
  assert.equal(errorHeader.errorCode, '12');
  assert.equal(errorHeader.errorMessage, 'missing handler');
  assert.equal(Object.hasOwn(errorHeader, 'status'), false);

  const responseHeader = JSON.parse(Buffer.from(
    envelope.encodeChannelReplyParts(request, { value: 'ok' })[0]
  ).toString('utf8'));
  assert.equal(responseHeader.kind, 2);
  assert.equal(Object.hasOwn(responseHeader, 'errorCode'), false);
  assert.equal(Object.hasOwn(responseHeader, 'errorMessage'), false);
  assert.equal(Object.hasOwn(responseHeader, 'status'), false);
});

test('RL-D4 checks the decoded failure and message before a successful follow-up', () => {
  const scenario = read('Client/Scenarios/rl-d4-missing-request-handler-scenario.ts');
  assert.match(scenario, /ensure\(failed\.failed/);
  assert.match(scenario, /failed\.failureMessage\.includes\(/);
  assert.match(scenario, /followUp\.value === 'profile:fast'/);
});

test('RL-D5 sustains mixed work across clients and checks drift plus cleanup', () => {
  const scenario = read('Client/Scenarios/rl-d5-mixed-burst-scenario.ts');
  const options = read('Client/Support/client-options.ts');
  const runner = read('run_e2e.sh');

  assert.match(runner, /RL_D5_DURATION_SECONDS="\$\{RL_D5_DURATION_SECONDS:-120\}"/);
  assert.match(runner, /--soak-duration-seconds "\$RL_D5_DURATION_SECONDS"/);
  assert.match(options, /soakDurationMs/);
  assert.match(options, /soakDurationSeconds < 120/);
  assert.match(scenario, /options\.consumerUrls\.map/);
  assert.match(scenario, /while \(Date\.now\(\) < deadline\)/);
  assert.match(scenario, /await delay\(workloadIntervalMs\)/);
  assert.match(scenario, /firstHalfP95/);
  assert.match(scenario, /secondHalfP95/);
  assert.match(scenario, /confirmTailSends/);
  assert.match(scenario, /findProviderEvidenceMarkers/);
  assert.match(scenario, /\/profile\/request\/new-client/);
  assert.doesNotMatch(scenario, /Array\.from\(\{ length: 60 \}/);
});
