const assert = require('node:assert/strict');
const test = require('node:test');

const frameworkJson = require('../../packages/stream-wire/dist/internal/framework-json-v1');

const schema = {
  type: 'object',
  properties: { data: { type: 'bytes' } },
  required: ['data']
};

// Spec Payload ownership §2: a JSON bytes field is converted by the host's
// native base64 codec, not one JavaScript string operation per byte, and the
// canonical RFC 4648 check stays.
test('JSON bytes fields use the native base64 codec without per-byte string work', (t) => {
  const payload = Uint8Array.from({ length: 1024 }, (_, index) => index % 251);
  let perByteOperations = 0;
  const charCodeAt = String.prototype.charCodeAt;
  t.mock.method(String.prototype, 'charCodeAt', function (...args) {
    perByteOperations++;
    return charCodeAt.apply(this, args);
  });
  const fromCharCode = String.fromCharCode;
  t.mock.method(String, 'fromCharCode', (...codes) => {
    perByteOperations += codes.length;
    return fromCharCode(...codes);
  });
  const text = frameworkJson.stringifyFrameworkJsonV1({ data: payload }, schema);
  const decoded = frameworkJson.parseFrameworkJsonV1(text, {}, schema);
  t.mock.restoreAll();
  assert.deepEqual(decoded.data, payload);
  assert.ok(perByteOperations < 64, `string operations for 1024 bytes: ${perByteOperations}`);
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"data":"AQJ="}', {}, schema),
    /canonical padded RFC 4648 base64/
  );
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"data":"AQI"}', {}, schema),
    /padded RFC 4648 base64/
  );
});
