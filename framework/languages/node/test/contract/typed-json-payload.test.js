const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  parseFrameworkJsonV1
} = require('../../packages/stream-wire/dist/internal/framework-json-v1');
const fixture = path.resolve(__dirname, '../../../../test/fixtures/typed-json-payload.tsv');
for (const line of fs.readFileSync(fixture, 'utf8').split('\n')) {
  if (!line || line.startsWith('#')) continue;
  const [id, expected, shape, json] = line.split('\t');
  test(`shared typed JSON ${id}`, () => {
    const schema = {
      type: 'object',
      properties: { body: { type: 'bytes' } },
      required: shape === 'ctor' ? ['body'] : []
    };
    const decode = () => parseFrameworkJsonV1(json, {}, schema);
    if (expected === 'accept') assert.doesNotThrow(decode);
    else assert.throws(decode);
  });
}
