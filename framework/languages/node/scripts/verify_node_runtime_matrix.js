#!/usr/bin/env node
const childProcess = require('node:child_process');
const path = require('node:path');
const { ensureNodeBindingPackage } = require('./ensure_node_binding_dist');

const nodeRoot = path.resolve(__dirname, '..');
const gate = path.join(nodeRoot, 'scripts/run_node_runtime_gate.js');
const currentMajor = Number(process.versions.node.split('.')[0]);

ensureNodeBindingPackage();
runNodeMajor(20);
runNodeMajor(22);

function runNodeMajor(major) {
  console.log(`== Node ${major} runtime gate ==`);
  if (currentMajor === major) {
    run(process.execPath, [gate], major);
    return;
  }
  run('npx', ['-y', `node@${major}`, gate], major);
}

function run(command, args, major) {
  const result = childProcess.spawnSync(command, args, {
    cwd: nodeRoot,
    stdio: 'inherit',
    env: {
      ...process.env,
      ZLINK_EXPECT_NODE_MAJOR: String(major)
    }
  });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}
