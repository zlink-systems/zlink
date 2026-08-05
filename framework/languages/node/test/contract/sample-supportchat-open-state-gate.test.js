const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('SupportChat preserves the domain conversation state through the open response chain', () => {
  const contracts = read('samples/SupportChat.Ts/Shared/Contracts/messages.ts');
  const api = read('samples/SupportChat.Ts/Server/Api/Handlers/open-conversation-handler.ts');
  const entry = read(
    'samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/EntrySpot/support-entry-handlers.ts'
  );
  const conversationSpot = read(
    'samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts'
  );
  const actor = read(
    'samples/SupportChat.Ts/Server/Support/Infrastructure/ZLink/Actors/support-user-actor.ts'
  );

  assert.match(contracts, /type OpenConversationApiRes = \{ conversationId: string; status: ConversationStatus \};/);
  assert.match(api, /\.create\(SampleNames\.conversationSpotType\)/);
  assert.match(api, /\.inMesh\(SampleNames\.meshName\)/);
  assert.match(entry, /actor\.scheduleConversationJoin\(new JoinSupportConversation\(/);
  assert.match(actor, /this\.context\.joinSpot\([\s\S]*?\)\.defer\(\)/);
  assert.match(actor, /JoinConversationFailedNotify/);
  assert.match(api, /status:\s*ConversationStatuses\.WaitingForAgent/);
  assert.doesNotMatch(api, /requestToChannel|supportChannel|nodeRid/);
  assert.match(conversationSpot, /this\.assignments\.assignNextAgent\(\)/);
});
