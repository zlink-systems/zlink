const childProcess = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '..', '..');

test('node framework release gate declares required ABI and runtime matrix', () => {
  childProcess.execFileSync(process.execPath, ['scripts/verify_node_abi_matrix.js'], {
    cwd: workspaceRoot,
    stdio: 'inherit'
  });
});
