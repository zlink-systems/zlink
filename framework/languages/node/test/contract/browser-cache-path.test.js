const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const { syncBuiltinESMExports } = require('node:module');
const path = require('node:path');
const test = require('node:test');

test('Chromium installer and connector driver share the default cache and preserve overrides', async (t) => {
  const original = process.env.PLAYWRIGHT_BROWSERS_PATH;
  const originalExitCode = process.exitCode;
  let installedEnvironment;
  t.mock.method(childProcess, 'spawnSync', (_executable, args, options) => {
    assert.equal(args.at(-1), 'chromium');
    installedEnvironment = options.env;
    return { status: 0 };
  });
  syncBuiltinESMExports();
  try {
    const workspaceRoot = path.resolve(__dirname, '../..');
    for (const [index, override] of [undefined, path.join(workspaceRoot, '.cache/custom')].entries()) {
      if (override === undefined) delete process.env.PLAYWRIGHT_BROWSERS_PATH;
      else process.env.PLAYWRIGHT_BROWSERS_PATH = override;
      await import(`../../scripts/browser-e2e/install-chromium.mjs?cache-test=${index}`);
      await import(`../../scripts/browser-e2e/connector-driver.mjs?cache-test=${index}`);
      const expected = override ?? path.join(workspaceRoot, '.cache/ms-playwright');
      assert.equal(installedEnvironment.PLAYWRIGHT_BROWSERS_PATH, expected);
      assert.equal(process.env.PLAYWRIGHT_BROWSERS_PATH, expected);
    }
  } finally {
    if (original === undefined) delete process.env.PLAYWRIGHT_BROWSERS_PATH;
    else process.env.PLAYWRIGHT_BROWSERS_PATH = original;
    process.exitCode = originalExitCode;
    t.mock.restoreAll();
    syncBuiltinESMExports();
  }
});
