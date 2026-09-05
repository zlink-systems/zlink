const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

for (const mode of ['normal', 'shared', 'failure', 'deadline']) {
  test(`Nest app.close releases owned resources and exits naturally: ${mode}`, () => {
    const result = spawnSync(process.execPath, [path.join(__dirname, 'fixtures/nestjs-shutdown-process.js'), mode], {
      encoding: 'utf8', timeout: 10000, killSignal: 'SIGKILL'
    });
    assert.equal(result.error, undefined, result.stderr);
    assert.equal(result.signal, null, result.stderr);
    assert.equal(result.status, 0, result.stderr);
    const report = JSON.parse(result.stdout.trim());
    assert.equal(report.handles, 0);
    assert.equal(report.timers, 0);
  });
}
