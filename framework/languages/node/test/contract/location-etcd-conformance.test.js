const test = require('node:test');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
if (process.env.ZLINK_ETCD_CONFORMANCE === '1') {
  test('Node etcd Location Store conformance', { timeout: 1200000 }, async () => {
    const { runConformance } = await import(
      pathToFileURL(path.resolve(__dirname, '../../../../../scripts/etcd/conformance-runner.mjs'))
        .href
    );
    await runConformance({ allowUnverified: process.env.ZLINK_ETCD_ALLOW_UNVERIFIED === '1' });
  });
}
