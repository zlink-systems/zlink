const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const contracts = path.resolve(__dirname, '../../packages/framework/src/contracts');
const read = relative => fs.readFileSync(path.join(contracts, relative), 'utf8');
const body = (source, name) => {
  const match = source.match(new RegExp(`interface ${name}\\b[^\\{]*\\{([\\s\\S]*?)\\n\\}`));
  assert.ok(match, name);
  return match[1];
};

test('send, reply and relay declarations have no caller cancellation or timeout', () => {
  for (const [file, name] of [
    ['Channels/Calls.ts', 'ZLinkSendCall'],
    ['Spots/Contracts.ts', 'ZLinkSpotSendCall'],
    ['Actors/ZLinkActorClient.ts', 'ZLinkActorSendCall'],
    ['Streams/BoundSessionContracts.ts', 'ZLinkBoundSessionSendCall'],
    ['Streams/IZLinkSession.ts', 'ZLinkSessionSendCall'],
    ['Streams/IZLinkSession.ts', 'ZLinkSessionReplyCall']
  ]) {
    const declaration = body(read(file), name);
    assert.match(declaration, /submit\(\): Promise<void>/, name);
    assert.doesNotMatch(declaration, /AbortSignal|timeout\(/, name);
  }
  const actor = body(read('Streams/IZLinkSessionActor.ts'), 'ZLinkSessionActor');
  assert.doesNotMatch(actor.slice(0, actor.indexOf('notifyDisconnected')), /AbortSignal/);
});

test('only Classic fanout publisher configuration exposes send timeout', () => {
  assert.doesNotMatch(body(read('Configuration/Configs.ts'), 'ZLinkSocketConfig'), /sendTimeout/);
  assert.match(body(read('Configuration/Builders.ts'), 'ZLinkFanoutChannelBuilder'), /setSendTimeout\(timeoutMs: number\)/);
  assert.doesNotMatch(read('Configuration/Configs.ts'), /ZLinkSpotPublisherConfig/);
  const source = read('Configuration/RegistrationTypes.ts');
  for (const name of ['ZLinkClientCapabilityOptions', 'ZLinkRouteMeshChannelOptions', 'ZLinkRouteChannelOptions']) {
    assert.doesNotMatch(body(source, name), /sendTimeout/);
  }
  assert.match(body(source, 'ZLinkChannelOptions'), /sendTimeoutMs/);
});
