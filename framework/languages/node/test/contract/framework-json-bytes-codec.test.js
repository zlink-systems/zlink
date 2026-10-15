const assert = require('node:assert/strict');
const test = require('node:test');

const frameworkJson = require('../../packages/stream-wire/dist/internal/framework-json-v1');

const schema = {
  type: 'object',
  properties: { data: { type: 'bytes' } },
  required: ['data']
};

// Spec Payload ownership §2: a JSON bytes field is encoded once and decoded
// once. The canonical RFC 4648 check reads the padding bits instead of
// re-encoding the decoded bytes, and the decoded bytes are not copied again.
// stream-wire stays browser-portable, so each direction keeps one portable
// per-byte pass.
test('JSON bytes fields encode and decode once with the canonical check on the padding bits', (t) => {
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
  // One encode pass and one decode pass over 1024 bytes, plus the JSON scan.
  assert.ok(perByteOperations < 2100, `string operations for 1024 bytes: ${perByteOperations}`);
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"data":"AQJ="}', {}, schema),
    /canonical padded RFC 4648 base64/
  );
  assert.throws(
    () => frameworkJson.parseFrameworkJsonV1('{"data":"AQI"}', {}, schema),
    /padded RFC 4648 base64/
  );
});
