const assert = require('node:assert/strict');
const test = require('node:test');
const frameworkJson = require('../../packages/stream-wire/dist/internal/framework-json-v1');

test('escaped property aliases still participate in duplicate detection', () => {
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"name":1,"\\u006eame":2}'),
    /duplicate property 'name'/
  );
});

test('an invalid escaped value stops scanning before the next property callback', () => {
  const rejectedNames = [];

  assert.throws(
    () =>
      frameworkJson.parseFrameworkJsonV1(
        '{"first":"bad\\x","second":2}',
        { rejectPropertyName: (name) => (rejectedNames.push(name), false) }
      ),
    SyntaxError
  );
  assert.deepEqual(rejectedNames, ['first']);
});

test('raw control characters in property names reach the property callback before native rejection', () => {
  const rejectedNames = [];

  assert.throws(
    () =>
      frameworkJson.parseFrameworkJsonV1('{"bad\nname":1}', {
        rejectPropertyName: (name) => (rejectedNames.push(name), false)
      }),
    SyntaxError
  );
  assert.deepEqual(rejectedNames, ['bad\nname']);
});

test('string delimiter search remains correct during a recursive property callback', () => {
  const rejectedNames = [];
  const result = frameworkJson.parseFrameworkJsonV1('{"outer":"prefix","tail":["a","b"]}', {
    rejectPropertyName: (name) => {
      rejectedNames.push(name);
      if (name === 'outer') {
        assert.deepEqual(frameworkJson.parseFrameworkJsonV1('{"nested":"inside"}'), {
          nested: 'inside'
        });
      }
      return false;
    }
  });

  assert.deepEqual(result, { outer: 'prefix', tail: ['a', 'b'] });
  assert.deepEqual(rejectedNames, ['outer', 'tail']);
});

test('unterminated strings retain the scanner end offset', () => {
  const text = '{"key":"unfinished';

  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1(text),
    new SyntaxError(`framework-json-v1 expected unterminated string at offset ${text.length}.`)
  );
});

test('native parsing preserves Unicode, negative zero, and reviver behavior', () => {
  const reviverCalls = [];
  const result = frameworkJson.parseFrameworkJsonV1(
    '{"text":"\\uD83D\\uDE03","negativeZero":-0,"nested":{"value":"é"}}',
    {},
    undefined,
    function (key, value) {
      reviverCalls.push(key);
      return key === 'value' ? `${value}!` : value;
    }
  );

  assert.deepEqual(result, {
    text: '😃',
    negativeZero: -0,
    nested: { value: 'é!' }
  });
  assert.ok(Object.is(result.negativeZero, -0));
  assert.deepEqual(reviverCalls, ['text', 'negativeZero', 'value', 'nested', '']);
});
