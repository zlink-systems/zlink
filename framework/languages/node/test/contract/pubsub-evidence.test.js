const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const evidenceSource = path.join(nodeRoot, 'e2e/PubSub/Client/Support/evidence.ts');

test('PS-A1 evidence excludes warm-up values and requires matching delivery order', () => {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-pubsub-evidence-'));
  try {
    compileEvidence(outputRoot);
    const { commonContiguousSequence } = require(path.join(outputRoot, 'evidence.js'));
    const common = [
      'event|run=run-1|topic=orders|seq=100|value=warmup-100',
      'event|run=run-1|topic=orders|seq=1000|value=measure-1000',
      'event|run=run-1|topic=orders|seq=1001|value=measure-1001',
      'event|run=run-1|topic=orders|seq=1002|value=measure-1002'
    ];

    assert.deepEqual(
      commonContiguousSequence([common, common], 'run-1', 'orders', 100, 102, 'measure-'),
      []
    );
    assert.deepEqual(
      commonContiguousSequence([
        common,
        [common[1], common[3], common[2]]
      ], 'run-1', 'orders', 1000, 1002, 'measure-'),
      [1000, 1001]
    );
  } finally {
    fs.rmSync(outputRoot, { recursive: true, force: true });
  }
});

function compileEvidence(outputRoot) {
  const result = childProcess.spawnSync(
    process.execPath,
    [
      path.join(nodeRoot, 'node_modules/typescript/bin/tsc'),
      '--target', 'ES2022',
      '--module', 'commonjs',
      '--moduleResolution', 'node',
      '--skipLibCheck',
      '--outDir', outputRoot,
      evidenceSource
    ],
    { cwd: nodeRoot, encoding: 'utf8' }
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
}
