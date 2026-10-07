const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const source = fs.readFileSync(path.join(__dirname, '../Client/main.ts'), 'utf8');
const start = source.indexOf('    const settledDiagonal = await second');
const end = source.indexOf("    console.log('scenario ZW-B1 passed');", start);
assert.ok(start >= 0 && end > start);
const compiled = ts.transpileModule(
  `async function observe(second, boundary, joined, joinedSecond, zlinkStreamAssert, PacketNames) {\n${source.slice(start, end)}\n} exports.observe = observe;`,
  { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }
).outputText;
const scenario = {};
new Function('exports', compiled)(scenario);

test('ZW-B1 compares only successive diagonal-zone snapshots', async () => {
  const packets = [
    { zoneId: 'zone-nw', tick: 120, players: [{ playerId: 'player-a1' }] },
    ...[40, 41, 42, 43].map((tick) => ({
      zoneId: 'zone-ne',
      tick,
      players: [{ playerId: 'player-a3' }]
    }))
  ];
  const consumed = [];
  const second = {
    waitFor() {
      return {
        where(predicate) {
          return {
            timeout() {
              return this;
            },
            async submit() {
              const index = packets.findIndex((payload) => predicate({ payload }));
              assert.ok(index >= 0, 'expected diagonal-zone status');
              const [payload] = packets.splice(index, 1);
              consumed.push(`${payload.zoneId}:${payload.tick}`);
              return { payload };
            }
          };
        }
      };
    }
  };
  await scenario.observe(
    second,
    { diagonalZoneId: 'zone-ne' },
    { playerId: 'player-a1' },
    { playerId: 'player-a3' },
    {
      ensure(value, message) {
        assert.ok(value, message);
      }
    },
    { zoneStateNotify: 'ZoneStateNotify' }
  );
  assert.deepEqual(consumed, ['zone-ne:40', 'zone-ne:41', 'zone-ne:42', 'zone-ne:43']);
  assert.deepEqual(
    packets.map((packet) => packet.zoneId),
    ['zone-nw']
  );
});
