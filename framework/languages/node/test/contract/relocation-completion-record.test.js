const assert = require('node:assert/strict');
const test = require('node:test');
const { createHash } = require('node:crypto');
const golden = require('../../../../runtime/protocol/golden/relocation-completion-record-v1.json');
const codec = require('../../packages/framework/dist/runtime/locations/relocation-completion-record');
for (const entry of golden.cases) {
  test(`completion record golden: ${entry.name}`, () => {
    const bytes = Buffer.from(entry.bytesBase64, 'base64');
    if (entry.expect === 'reject') {
      assert.throws(() => codec.decodeRelocationCompletionRecord(bytes), /DataLost/);
      return;
    }
    const record = codec.decodeRelocationCompletionRecord(bytes);
    assert.deepEqual(codec.encodeRelocationCompletionRecord(record), bytes);
    assert.equal(createHash('sha256').update(bytes).digest('hex'), entry.sha256);
    assert.equal(BigInt(record.entries.length), BigInt(entry.completedResults));
    assert.equal(
      BigInt(record.entries.filter((e) => e.delivery === 'pending').length),
      BigInt(entry.pendingDeliveries)
    );
  });
}
