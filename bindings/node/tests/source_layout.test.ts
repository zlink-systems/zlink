'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

function collectTypeScriptFiles(root: string): string[] {
  return fs.readdirSync(root, { withFileTypes: true }).flatMap((entry) => {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) return collectTypeScriptFiles(fullPath);
    return entry.isFile() && entry.name.endsWith('.ts') ? [fullPath] : [];
  });
}

function collectScriptFiles(root: string): string[] {
  return fs.readdirSync(root, { withFileTypes: true }).flatMap((entry) => {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) return collectScriptFiles(fullPath);
    return entry.isFile() && /\.(?:js|ts)$/.test(entry.name) ? [fullPath] : [];
  });
}

function runtimeSpecifiers(source: string): string[] {
  const specifiers: string[] = [];
  const patterns = [
    /(?:import|export)\s+(?:[\s\S]*?\s+from\s+)?['"]([^'"]+)['"]/g,
    /\brequire\s*\(\s*['"]([^'"]+)['"]\s*\)/g,
    /\bimport\s*\(\s*['"]([^'"]+)['"]\s*\)/g
  ];

  for (const pattern of patterns) {
    for (const match of source.matchAll(pattern)) {
      const normalized = match[1].replaceAll('\\', '/').toLowerCase();
      const delimited = `/${normalized}/`;
      if (delimited.includes('/runtime/') || delimited.includes('/native/')) {
        specifiers.push(match[1]);
      }
    }
  }
  return specifiers;
}

function forbiddenPackageExports(exportsValue: unknown): string[] {
  if (!exportsValue || typeof exportsValue !== 'object') return [];
  return Object.keys(exportsValue).filter((key) => key !== '.');
}

test('contract source does not import or re-export runtime implementation', () => {
  assert.deepEqual(runtimeSpecifiers("import { Message } from '../messaging/message';"), []);
  assert.deepEqual(runtimeSpecifiers("import { Native } from '../../runtime/native/native';"), [
    '../../runtime/native/native'
  ]);
  assert.deepEqual(runtimeSpecifiers("export { Socket } from '../runtime/sockets/socket';"), [
    '../runtime/sockets/socket'
  ]);
  assert.deepEqual(runtimeSpecifiers("const addon = require('../../native/addon');"), [
    '../../native/addon'
  ]);

  const contractRoot = path.resolve(__dirname, '../../src/zlink/contracts');
  const violations = collectTypeScriptFiles(contractRoot).flatMap((file) =>
    runtimeSpecifiers(fs.readFileSync(file, 'utf8')).map((specifier) => ({ file, specifier }))
  );
  assert.deepEqual(violations, []);
});

test('package exports expose only the public root', () => {
  assert.deepEqual(forbiddenPackageExports({ '.': './dist/index.js' }), []);
  assert.deepEqual(
    forbiddenPackageExports({ '.': './dist/index.js', './runtime/native': './dist/runtime/native.js' }),
    ['./runtime/native']
  );

  const packageJson = JSON.parse(
    fs.readFileSync(path.resolve(__dirname, '../../package.json'), 'utf8')
  ) as { exports?: unknown };
  assert.deepEqual(forbiddenPackageExports(packageJson.exports), []);
});

test('Node async surfaces use Core completion callbacks and have no readiness admission files', () => {
  const nativeRoot = path.resolve(__dirname, '../../native/src');
  const sourceRoot = path.resolve(__dirname, '../../src/zlink');
  const gyp = fs.readFileSync(path.resolve(__dirname, '../../binding.gyp'), 'utf8');
  const nativeBridge = fs.readFileSync(path.join(nativeRoot, 'addon_core.cc'), 'utf8');
  const requestBridge = fs.readFileSync(path.join(nativeRoot, 'addon_request_callbacks.cc'), 'utf8');
  const socketBinding = fs.readFileSync(
    path.join(sourceRoot, 'runtime/native/binding_socket.ts'),
    'utf8'
  );
  const sendCompletion = fs.readFileSync(
    path.join(sourceRoot, 'runtime/messaging/send_completion.ts'),
    'utf8'
  );

  assert.equal(fs.existsSync(path.join(nativeRoot, 'addon_routed_admission.cc')), false);
  assert.equal(fs.existsSync(path.join(sourceRoot, 'runtime/sockets/routed_admission.ts')), false);
  assert.equal(fs.existsSync(path.join(sourceRoot, 'runtime/sockets/publisher_admission.ts')), false);
  assert.equal(fs.existsSync(path.join(sourceRoot, 'runtime/messaging/request_progress.ts')), false);
  assert.equal(gyp.includes('addon_routed_admission.cc'), false);
  for (const symbol of [
    'zlink_send_complete_handler',
    'zlink_send_async',
    'napi_create_threadsafe_function',
    'socket_send_async',
    'dealer_request',
    'router_request'
  ]) {
    assert.ok(nativeBridge.includes(symbol), symbol);
  }
  assert.ok(requestBridge.includes('napi_call_threadsafe_function'));
  assert.ok(socketBinding.includes('socketSendCompletionHandler'));
  assert.ok(socketBinding.includes('socketSendAsync'));
  assert.ok(sendCompletion.includes('new Map<bigint, PendingSend>()'));
  assert.equal(nativeBridge.includes('send_ready'), false);
  assert.equal(nativeBridge.includes('routed_send_ready'), false);
  assert.equal(socketBinding.includes('sendReady'), false);
});

test('bindings samples stay on the Core 0.13.0 raw socket boundary', () => {
  const sampleRoots = [
    path.resolve(__dirname, '../../samples'),
    path.resolve(__dirname, '../../../javascript/samples')
  ];
  const forbidden = [
    /\bcreateMeshNode\s*\(/,
    /\bcreateActor\s*\(/,
    /\bcreateSpot\s*\(/,
    /\bjoinActorSpot\s*\(/,
    /\bbindActor\s*\(/
  ];
  const violations = sampleRoots.flatMap((root) =>
    collectScriptFiles(root).flatMap((file) => {
      const source = fs.readFileSync(file, 'utf8');
      return forbidden.some((pattern) => pattern.test(source)) ? [file] : [];
    })
  );

  assert.deepEqual(
    violations,
    [],
    'Actor, Spot and session service examples belong to framework samples'
  );
});
