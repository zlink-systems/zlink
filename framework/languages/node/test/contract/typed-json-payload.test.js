const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  parseFrameworkJsonV1,
  stringifyFrameworkJsonV1
} = require('../../packages/stream-wire/dist/internal/framework-json-v1');
const fixture = path.resolve(__dirname, '../../../../test/fixtures/typed-json-payload.tsv');
for (const line of fs.readFileSync(fixture, 'utf8').split('\n')) {
  if (!line || line.startsWith('#')) continue;
  const [id, expected, shape, json] = line.split('\t');
  test(`shared typed JSON ${id}`, () => {
    if (shape.startsWith('encode-')) {
      const child = {
        type: 'object',
        properties: { text: { type: 'string' } },
        required: ['text']
      };
      const schema = {
        type: 'object',
        properties:
          shape === 'encode-nullable'
            ? {
                text: { type: 'nullable', value: { type: 'string' } },
                child: { type: 'nullable', value: child }
              }
            : { wireValue: shape === 'encode-string' ? { type: 'string' } : child },
        required: shape === 'encode-nullable' ? ['text', 'child'] : ['wireValue']
      };
      // Exercise both the portable profile and the Node framework's byte adapter.
      for (const codec of [
        { stringifyFrameworkJsonV1 },
        require('../../packages/framework/dist/runtime/messaging/framework-json-v1')
      ]) {
        let wire;
        let failure;
        try {
          wire = codec.stringifyFrameworkJsonV1(JSON.parse(json), schema);
        } catch (error) {
          failure = error;
        }
        console.log(`ENCODE_CONTRACT ${id} wire=${wire} error=${failure?.message}`);
        if (expected === 'accept') {
          assert.ifError(failure);
          assert.equal(wire, json);
        } else {
          assert.ok(failure instanceof TypeError);
          assert.equal(
            failure.message,
            `framework-json-v1 value '$.wireValue' must be ${shape === 'encode-string' ? 'a string' : 'an object'}.`
          );
        }
      }
      return;
    }
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
