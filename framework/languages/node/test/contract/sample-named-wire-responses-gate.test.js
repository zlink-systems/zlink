const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('sample sessions construct named wire response messages', () => {
  const cases = [
    {
      contract: 'samples/SupportChat.Ts/Shared/Contracts/messages.ts',
      source: 'samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts',
      response: 'AuthenticateRes'
    },
    {
      contract: 'samples/DeliveryDispatch.Ts/Shared/Contracts/messages.ts',
      source: 'samples/DeliveryDispatch.Ts/Server/Session/customer-session.ts',
      response: 'SubscribeDeliveryRes'
    },
    {
      contract: 'samples/GameQuest.Ts/Shared/Contracts/messages.ts',
      source: 'samples/GameQuest.Ts/Server/GameApi/game-api-session.ts',
      response: 'JoinSessionRes'
    }
  ];

  for (const item of cases) {
    const contract = read(item.contract);
    const source = read(item.source);
    assert.match(contract, new RegExp(`class ${item.response} \\{`));
    assert.match(source, new RegExp(`reply\\(new ${item.response}\\(`));
  }
});
