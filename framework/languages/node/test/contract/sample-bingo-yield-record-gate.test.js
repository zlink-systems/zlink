const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('Bingo retrieves and reports player records through yielded API requests', () => {
  const proto = read('samples/Bingo.Ts/Shared/Contracts/bingo_messages.proto');
  assert.match(proto, /message GetPlayerRecordReq/);
  assert.match(proto, /message ReportBingoResultReq/);
  assert.match(proto, /message BingoPlayerState[\s\S]*int32 wins[\s\S]*int32 losses/);

  const room = read(
    'samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts'
  );
  assert.match(room, /requestToChannel\(\s*SampleNames\.apiChannel,\s*new GetPlayerRecordReq/);
  assert.match(room, /requestToChannel\(\s*SampleNames\.apiChannel,\s*new ReportBingoResultReq/);
  assert.match(room, /new GetPlayerRecordReq\(\{ actorId \}\)[\s\S]*?\.yield<GetPlayerRecordRes>\(\)/);
  assert.doesNotMatch(room, /new GetPlayerRecordReq\(\{ actorId \}\)[\s\S]*?\.submit<GetPlayerRecordRes>\(\)/);
  assert.match(room, /!this\.playerIds\.has\(actorId\) \|\| this\.snapshot\(\)\.status === BingoRoomStatus\.Finished/);
  assert.match(room, /bingo-record fetched/);
  assert.match(room, /bingo-record reported/);

  const client = read('samples/Bingo.Ts/Client/bingo-client-scenario.ts');
  assert.match(client, /player\.wins === 0/);
  assert.match(client, /player\.losses === 0/);
});
