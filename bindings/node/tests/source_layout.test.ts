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

test('Node requests and writable send retries use pull completion without callback bridges', () => {
  const nativeRoot = path.resolve(__dirname, '../../native/src');
  const sourceRoot = path.resolve(__dirname, '../../src/zlink');
  const gyp = fs.readFileSync(path.resolve(__dirname, '../../binding.gyp'), 'utf8');
  const nativeBridge = fs.readFileSync(path.join(nativeRoot, 'addon_core.cc'), 'utf8');
  const socketBinding = fs.readFileSync(
    path.join(sourceRoot, 'runtime/native/binding_socket.ts'), 'utf8');
  const completionOwner = fs.readFileSync(
    path.join(sourceRoot, 'runtime/messaging/completion_owner.ts'), 'utf8');
  const poller = fs.readFileSync(
    path.join(sourceRoot, 'runtime/eventing/poller.ts'), 'utf8');

  assert.equal(fs.existsSync(path.join(nativeRoot, 'addon_request_callbacks.cc')), false);
  assert.equal(fs.existsSync(path.join(nativeRoot, 'addon_request_callbacks.h')), false);
  assert.equal(gyp.includes('addon_request_callbacks.cc'), false);
  for (const symbol of [
    'zlink_completion_recv',
    'zlink_completion_close',
    'socket_submit_send',
    'socket_submit_request',
    'socket_completion_recv',
  ]) assert.ok(nativeBridge.includes(symbol), symbol);
  for (const removed of [
    'zlink_send_async',
    'zlink_send_complete_handler',
    'napi_create_threadsafe_function',
  ]) assert.equal(nativeBridge.includes(removed), false, removed);
  assert.ok(socketBinding.includes('socketCompletionRecv'));
  assert.ok(socketBinding.includes('socketSubmitSend'));
  assert.ok(completionOwner.includes('class CompletionEntry'));
  assert.ok(completionOwner.includes('byToken'));
  assert.ok(completionOwner.includes('byId'));
  assert.ok(completionOwner.includes('sendRetries'));
  assert.ok(completionOwner.includes('COMPLETION_WRITABLE'));
  assert.ok(completionOwner.includes('PollEventFlag.PollOut'));
  assert.ok(completionOwner.includes('awaitWritable'));
  assert.ok(completionOwner.includes('transferToPublic'));
  assert.equal(completionOwner.includes('COMPLETION_SEND'), false);
  assert.equal(completionOwner.includes('setTimeout('), false);
  assert.ok(poller.includes('POLLER_SOURCE_SOCKET'));
  assert.ok(poller.includes('_socketRegistrationsByToken'));
  assert.ok(poller.includes('owner.drain(this)'));
  assert.ok(nativeBridge.includes('completion_close_guard_t guard'));
});

test('native multipart replies use inline staging without changing rejection ownership', () => {
  const nativeBridge = fs.readFileSync(
    path.resolve(__dirname, '../../native/src/addon_core.cc'), 'utf8'
  );
  for (const symbol of ['napi_value socket_reply']) {
    const start = nativeBridge.indexOf(symbol);
    assert.ok(start >= 0, symbol);
    const body = nativeBridge.slice(start, nativeBridge.indexOf('\nnapi_value ', start + symbol.length));
    assert.ok(body.includes('small_msg_storage_t parts'), `${symbol} inline storage`);
    assert.ok(body.includes('parts.release ()'), `${symbol} must not double-close rejected slots`);
    assert.equal(body.includes('std::vector<zlink_msg_t> parts'), false,
      `${symbol} must not allocate a vector for multipart replies`);
  }
});

test('bindings samples stay on the Core 0.16 raw socket boundary', () => {
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
