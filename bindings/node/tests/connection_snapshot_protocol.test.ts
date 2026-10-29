// SPDX-License-Identifier: MPL-2.0
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const os = require('node:os');
const { spawnSync } = require('node:child_process');
const dist = path.resolve(__dirname, '../../dist');

// Controlled JavaScript projection: this does not claim Core emitted unknown enums.
test('snapshot projection preserves opaque uint64 values and rejects unknown enums outside config translation', () => {
  const nativePath = path.join(dist, 'zlink/runtime/native/native.js');
  const saved = require.cache[nativePath];
  let rows: any[] = [];
  let queryError: any;
  require.cache[nativePath] = { id: nativePath, filename: nativePath, loaded: true,
    exports: { requireNative: () => ({ socketConnectionAttemptsSnapshot: () => {
      if (queryError) throw queryError;
      return rows;
    } }) } } as any;
  try {
    const { ConnectableSocket } = require(path.join(dist, 'zlink/runtime/sockets/socket_base.js'));
    const { ConfigError, ConfigResult } = require(path.join(dist, 'zlink/contracts/errors/errors.js'));
    const socket = Object.create(ConnectableSocket.prototype);
    const row = { endpoint: 'tcp://owned', intentId: 0xffffffffffffffffn,
      revision: 0x8000000000000000n, lastFailureId: 0xfedcba9876543210n,
      state: 2, lastFailure: { kind: 1, stage: 2, normalizedErrno: -123, disconnectReason: 0xffffffff } };
    rows = [row];
    assert.deepEqual(socket.connectionAttemptsSnapshot(), [row]);
    for (const [owner, field] of [[row, 'state'], [row.lastFailure, 'kind'], [row.lastFailure, 'stage']] as any[]) {
      const previous = owner[field];
      owner[field] = 999;
      assert.throws(() => socket.connectionAttemptsSnapshot(), (error: any) =>
        error.constructor === Error && /connection snapshot protocol violation/.test(error.message));
      owner[field] = previous;
    }
    queryError = Object.assign(new Error('query failed'), { nativeResult: ConfigResult.InvalidHandle, nativeErrno: 0 });
    assert.throws(() => socket.connectionAttemptsSnapshot(), (error: any) =>
      error instanceof ConfigError && error.result === ConfigResult.InvalidHandle);
  } finally {
    if (saved) require.cache[nativePath] = saved;
    else delete require.cache[nativePath];
  }
});

// JavaScript surface/failure aggregation test; this does not load native code.
test('JavaScript candidates without the snapshot getter preserve both errors and cache the failure', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-old-addon-'));
  try {
    for (const name of ['development', 'prebuilt']) {
      fs.writeFileSync(path.join(directory, `${name}.js`), 'global.fixtureLoads = (global.fixtureLoads || 0) + 1; module.exports = {};');
    }
    const script = `
      const assert = require('node:assert/strict');
      const paths = require(${JSON.stringify(path.join(dist, 'zlink/runtime/native/native_load_paths.js'))});
      paths.nativeLoadPaths = () => ({ packageRoot: ${JSON.stringify(directory)},
        buildAddon: ${JSON.stringify(path.join(directory, 'development.js'))},
        prebuiltDir: ${JSON.stringify(directory)}, prebuiltAddon: ${JSON.stringify(path.join(directory, 'prebuilt.js'))} });
      paths.prepareDevelopmentRuntimeLink = () => {};
      paths.preparePrebuiltRuntimePath = () => {};
      const native = require(${JSON.stringify(path.join(dist, 'zlink/runtime/native/native.js'))});
      let firstError;
      assert.throws(() => native.requireNative(), error => {
        firstError = error;
        return /development.js.*socketConnectionAttemptsSnapshot is missing/.test(error.message)
        && /prebuilt.js.*socketConnectionAttemptsSnapshot is missing/.test(error.message);
      });
      assert.throws(() => native.requireNative(), error => error === firstError);
      assert.equal(global.fixtureLoads, 2);
      for (const name of ['development', 'prebuilt']) {
        const filename = require.resolve(require('node:path').join(${JSON.stringify(directory)}, name + '.js'));
        assert.equal(require.cache[filename].loaded, true);
      }
    `;
    const child = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
    assert.equal(child.status, 0, child.stderr);
    assert.equal(child.signal, null);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

test('fresh actual addon opens once and preserves native and helper export identity', () => {
  const script = `
    const assert = require('node:assert/strict');
    const helperPath = ${JSON.stringify(path.join(dist, 'zlink/runtime/native/native_load_paths.js'))};
    const paths = require(helperPath);
    const candidates = paths.nativeLoadPaths();
    paths.prepareDevelopmentRuntimeLink(candidates.packageRoot);
    const filename = require.resolve(candidates.buildAddon);
    const originalDlopen = process.dlopen;
    let opens = 0;
    process.dlopen = function(addon, target, flags) {
      if (target === filename) {
        opens++;
        assert.notEqual(addon, require.cache[helperPath]);
        assert.equal(require.cache[filename], addon);
        assert.equal(addon.loaded, false);
        if (process.platform !== 'win32') {
          assert.equal(flags, require('node:os').constants.dlopen.RTLD_NOW);
        }
      }
      return originalDlopen.apply(this, arguments);
    };
    try {
      const binding = paths.requireNativeAt(filename);
      assert.equal(typeof binding.socketConnectionAttemptsSnapshot, 'function');
      assert.equal(paths.requireNativeAt(filename), binding);
      assert.equal(require(filename), binding);
      assert.equal(require.cache[filename].exports, binding);
      assert.equal(require.cache[filename].loaded, true);
      assert.equal(require(helperPath), paths);
      assert.equal(typeof paths.nativeLoadPaths, 'function');
      assert.equal(typeof paths.requireNativeAt, 'function');
      assert.equal(opens, 1);
    } finally {
      process.dlopen = originalDlopen;
    }
  `;
  const child = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
  assert.equal(child.status, 0, child.stderr);
  assert.equal(child.signal, null);
});

test('failed actual addon opening removes its cache entry and parent edge before prebuilt fallback', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-failed-addon-'));
  const failed = path.join(directory, 'development.node');
  fs.writeFileSync(failed, 'invalid native addon');
  try {
    const script = `
      const assert = require('node:assert/strict');
      const helperPath = ${JSON.stringify(path.join(dist, 'zlink/runtime/native/native_load_paths.js'))};
      const paths = require(helperPath);
      const helper = require.cache[helperPath];
      const children = helper.children.slice();
      const candidates = paths.nativeLoadPaths();
      paths.prepareDevelopmentRuntimeLink(candidates.packageRoot);
      const prebuilt = candidates.buildAddon;
      const failed = ${JSON.stringify(failed)};
      const originalDlopen = process.dlopen;
      let openingError;
      process.dlopen = function(addon, target, flags) {
        try { return originalDlopen.apply(this, arguments); }
        catch (error) { if (target === failed) openingError = error; throw error; }
      };
      try {
        assert.throws(() => paths.requireNativeAt(failed), error =>
          error === openingError && error.code === 'ERR_DLOPEN_FAILED');
        assert.equal(require.cache[failed], undefined);
        assert.deepEqual(helper.children, children);
        assert.equal(require.cache[helperPath], helper);
        paths.nativeLoadPaths = () => ({ packageRoot: ${JSON.stringify(directory)},
          buildAddon: failed, prebuiltDir: require('node:path').dirname(prebuilt), prebuiltAddon: prebuilt });
        paths.prepareDevelopmentRuntimeLink = () => {};
        paths.preparePrebuiltRuntimePath = () => {};
        const native = require(${JSON.stringify(path.join(dist, 'zlink/runtime/native/native.js'))});
        const binding = native.requireNative();
        assert.equal(typeof binding.socketConnectionAttemptsSnapshot, 'function');
        assert.equal(native.requireNative(), binding);
        assert.equal(require(prebuilt), binding);
        assert.equal(require.cache[failed], undefined);
        assert.equal(helper.children.some(child => child.filename === failed), false);
      } finally {
        process.dlopen = originalDlopen;
      }
    `;
    const child = spawnSync(process.execPath, ['-e', script], { encoding: 'utf8' });
    assert.equal(child.status, 0, child.stderr);
    assert.equal(child.signal, null);
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});

// Ordinary JavaScript loading only; no claim about native initialization or Core symbols.
test('JavaScript getter surface uses ordinary require cache and exports', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-js-surface-'));
  const filename = path.join(directory, 'surface.js');
  fs.writeFileSync(filename, 'module.exports = { socketConnectionAttemptsSnapshot() {} };');
  try {
    const { requireNativeAt } = require(path.join(dist, 'zlink/runtime/native/native_load_paths.js'));
    const binding = requireNativeAt(filename);
    assert.equal(binding, require(filename));
    assert.equal(requireNativeAt(filename), binding);
  } finally {
    delete require.cache[filename];
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
