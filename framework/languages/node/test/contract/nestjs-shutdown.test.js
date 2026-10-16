const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

function runFixture(mode) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [path.join(__dirname, 'fixtures/nestjs-shutdown-process.js'), mode], {
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8').on('data', chunk => { stdout += chunk; });
    child.stderr.setEncoding('utf8').on('data', chunk => { stderr += chunk; });
    child.once('error', reject);
    child.once('close', (status, signal) => resolve({ status, signal, stdout, stderr }));
  });
}

for (const mode of ['normal', 'shared', 'failure', 'deadline']) {
  //  The child's own exit ends the test. Cold module loading under host load is not a shutdown
  //  property, so no wall-clock limit applies here; the runner's per-test timeout bounds a hang.
  test(`Nest app.close releases owned resources and exits naturally: ${mode}`, async () => {
    const result = await runFixture(mode);
    assert.equal(result.signal, null, result.stderr);
    assert.equal(result.status, 0, result.stderr);
    const report = JSON.parse(result.stdout.trim());
    assert.equal(report.handles, 0);
    assert.equal(report.timers, 0);
  });
}
