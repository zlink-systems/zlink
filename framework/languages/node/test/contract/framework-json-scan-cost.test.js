const assert = require('node:assert/strict');
const test = require('node:test');
const frameworkJson = require('../../packages/stream-wire/dist/internal/framework-json-v1');

// The strict pre-scan exists for duplicate property names, finite numbers and
// the property callback (spec Payload ownership §4). It reads the document
// through a String wrapper here so every per-character JavaScript read is
// counted; native searches, slices and the outer JSON.parse are not.
function parseCountingCharacterReads(text) {
  let reads = 0;
  const source = new String(text);
  const counted = new Proxy(source, {
    get(target, key) {
      if (typeof key === 'string' && /^[0-9]+$/u.test(key)) reads += 1;
      const value = Reflect.get(target, key);
      return typeof value === 'function' ? value.bind(target) : value;
    }
  });
  const value = frameworkJson.parseFrameworkJsonV1(counted);
  return { value, reads };
}

test('an escape-free string value costs the scan no per-character reads', () => {
  const short = parseCountingCharacterReads(JSON.stringify({ body: 'x'.repeat(1024) }));
  const long = parseCountingCharacterReads(JSON.stringify({ body: 'x'.repeat(4096) }));

  assert.deepEqual(short.value, { body: 'x'.repeat(1024) });
  assert.deepEqual(long.value, { body: 'x'.repeat(4096) });
  // Only structural tokens are read one by one, so value length does not matter.
  assert.equal(long.reads, short.reads);
});

test('strings after the first backslash still walk their escapes', () => {
  const escaped = parseCountingCharacterReads('{"a":"x","b":"\\"q\\"","c":"\\u0061"}');

  assert.deepEqual(escaped.value, { a: 'x', b: '"q"', c: 'a' });
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"a":"x","b":"\\"","\\u0062":1}'),
    /duplicate property 'b'/
  );
  assert.throws(() => frameworkJson.parseFrameworkJsonV1('{"a":"x","b":"\\u"}'), SyntaxError);
});

test('schema-known properties use no duplicate-name Set, including escaped names', (t) => {
  const schema = {
    type: 'object',
    required: ['items'],
    properties: {
      items: {
        type: 'array',
        items: {
          type: 'nullable',
          value: { type: 'object', required: ['id'], properties: { id: { type: 'int32' } } }
        }
      }
    }
  };
  const OriginalSet = Set;
  let allocations = 0;
  t.mock.method(globalThis, 'Set', class extends OriginalSet {
    constructor(...args) {
      super(...args);
      allocations++;
    }
  });
  const value = frameworkJson.parseFrameworkJsonV1('{"items":[{"\\u0069d":1},null,{"id":2}]}', {}, schema);
  assert.equal(allocations, 0);
  assert.deepEqual(value, { items: [{ id: 1 }, null, { id: 2 }] });
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"items":[{"id":1,"\\u0069d":2}]}', {}, schema),
    /duplicate property 'id'/
  );
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"items":[{"id":1,"x":1,"\\u0078":2}]}', {}, schema),
    /duplicate property 'x'/
  );
});
