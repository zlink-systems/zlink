const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('SupportChat distinguishes conversation message and join semantics', () => {
  const contracts = fs.readFileSync(path.join(
    root,
    'samples/SupportChat.Ts/Shared/Contracts/messages.ts'
  ), 'utf8');
  const client = fs.readFileSync(path.join(
    root,
    'samples/SupportChat.Ts/Client/supportchat-client-scenario.ts'
  ), 'utf8');

  assert.match(client, /customer1Joined\.payload\.conversationId === cid1/);
  assert.match(client, /customer1Joined\.payload\.state\.status === ConversationStatuses\.Active/);
  assert.match(client, /greeting1\.message\.conversationId === cid1/);
  assert.match(client, /greeting1\.message\.senderActorId === 'agent-1'/);
  assert.match(client, /greeting1\.message\.text === 'How can I help\?'/);
  assert.match(client, /reply1Push\.payload\.message\.conversationId === cid1/);
  assert.match(client, /reply1Push\.payload\.message\.text === 'Payment keeps failing\.'/);
  assert.match(client, /greeting2\.message\.conversationId === cid2/);
  assert.match(client, /greeting2Push\.payload\.conversationId === cid2/);
  assert.match(client, /greeting2Push\.payload\.message\.text === 'Let me check your account\.'/);
  assert.match(contracts, /class SetTypingMsg \{[\s\S]*?isTyping: boolean/);
  assert.match(contracts, /setTypingMsg: 'SetTypingMsg'/);
  assert.doesNotMatch(contracts, /SetTypingReq|setTypingReq/);
  assert.match(client, /PacketNames\.setTypingMsg/);
});

test('SupportChat arms lifecycle waits before the activity that starts their deadlines', () => {
  const client = fs.readFileSync(path.join(
    root,
    'samples/SupportChat.Ts/Client/supportchat-client-scenario.ts'
  ), 'utf8');

  const reconnectKeepaliveWait = client.indexOf('const reconnectKeepaliveTask = waitConversation');
  const reconnectKeepaliveRequest = client.indexOf('const reconnectKeepalive = await request');
  const firstIdleWait = client.indexOf('const firstIdleCustomer = waitConversation');
  const explicitClose = client.indexOf('const closed2 = await request');
  const secondIdleWait = client.indexOf('const secondIdleCustomer = waitConversation');
  const idleClosedWait = client.indexOf('const idleClosedCustomer = waitConversation');
  const resumeRequest = client.indexOf('const resumed = await request');

  assert.ok(reconnectKeepaliveWait >= 0 && reconnectKeepaliveWait < reconnectKeepaliveRequest);
  assert.ok(firstIdleWait >= 0 && firstIdleWait < explicitClose);
  assert.ok(secondIdleWait >= 0 && secondIdleWait < resumeRequest);
  assert.ok(idleClosedWait >= 0 && idleClosedWait < resumeRequest);
});
