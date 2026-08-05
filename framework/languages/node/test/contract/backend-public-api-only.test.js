const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const packageRoot = path.join(workspaceRoot, 'packages');

const forbiddenPatterns = [
  /bindings\/node\/src\//,
  /bindings\\node\\src\\/,
  /@zlink-systems\/zlink\/.+/,
  /zlink\/runtime\/native/,
  /zlink\/runtime\/public_bridge/,
  /requireNative/,
  /nativeHandle\s*\(/
];

test('framework packages only depend on binding public entry points', () => {
  const offenders = [];

  for (const file of sourceFiles(packageRoot)) {
    const content = fs.readFileSync(file, 'utf8');
    for (const pattern of forbiddenPatterns) {
      if (pattern.test(content)) {
        offenders.push(`${path.relative(workspaceRoot, file)} matches ${pattern}`);
      }
    }
  }

  assert.deepEqual(offenders, []);
});

test('Node E2E clients use only client-facing packages', () => {
  const e2eRoot = path.join(workspaceRoot, 'e2e');
  const forbidden = [
    /['"]@zlink-systems\/framework['"]/, 
    /['"]@zlink-systems\/framework-locations-redis['"]/, 
    /packages[\\/]framework[\\/]/,
    /runtime[\\/]internal/
  ];
  const offenders = [];

  for (const file of clientSourceFiles(e2eRoot)) {
    const content = fs.readFileSync(file, 'utf8');
    for (const pattern of forbidden) {
      if (pattern.test(content)) {
        offenders.push(`${path.relative(workspaceRoot, file)} matches ${pattern}`);
      }
    }
  }

  assert.deepEqual(offenders, []);
});

test('framework root public surface does not export backend adapter modules', () => {
  const rootIndex = fs.readFileSync(path.join(packageRoot, 'framework', 'src', 'index.ts'), 'utf8');

  assert.equal(rootIndex.includes("export * from './runtime/backend'"), false);
  assert.equal(rootIndex.includes('export * from "./runtime/backend"'), false);
});

test('framework contract surface does not alias binding concrete types', () => {
  const contracts = fs.readFileSync(path.join(packageRoot, 'framework', 'src', 'contracts', 'index.ts'), 'utf8');

  assert.equal(contracts.includes('@zlink-systems/zlink'), false);
  assert.equal(/import\('@zlink-systems\/zlink'\)/.test(contracts), false);
  assert.equal(/Binding(Message|ActorRef)/.test(contracts), false);
});

test('framework public options do not expose backend adapter factories', () => {
  const publicOptionFiles = [
    path.join(packageRoot, 'framework', 'src', 'runtime', 'host', 'index.ts')
  ];
  const offenders = [];

  for (const file of publicOptionFiles) {
    const content = fs.readFileSync(file, 'utf8');
    for (const block of exportedInterfaces(content)) {
      if (block.includes('backendAdapterFactory')) {
        offenders.push(path.relative(workspaceRoot, file));
      }
    }
  }

  assert.deepEqual(offenders, []);
});

test('framework runtime does not keep the removed registry implementation file', () => {
  assert.equal(
    fs.existsSync(path.join(packageRoot, 'framework', 'src', 'runtime', 'registry', 'index.ts')),
    false
  );
});

function exportedInterfaces(content) {
  return content.match(/export interface\s+\w+\s*\{[\s\S]*?\n\}/g) ?? [];
}

function* sourceFiles(root) {
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const absolute = path.join(root, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'dist' || entry.name === 'node_modules') {
        continue;
      }
      yield* sourceFiles(absolute);
      continue;
    }
    if (entry.isFile() && absolute.endsWith('.ts')) {
      yield absolute;
    }
  }
}

function* clientSourceFiles(root) {
  if (!fs.existsSync(root)) return;
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const absolute = path.join(root, entry.name);
    if (!entry.isDirectory() || entry.name === 'dist' || entry.name === 'node_modules') continue;
    for (const child of fs.readdirSync(absolute, { withFileTypes: true })) {
      if (!child.isDirectory() || (child.name !== 'Client' && child.name !== 'Contract')) continue;
      yield* clientTreeFiles(path.join(absolute, child.name));
    }
  }
}

function* clientTreeFiles(root) {
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const absolute = path.join(root, entry.name);
    if (entry.isDirectory()) {
      if (entry.name === 'dist' || entry.name === 'node_modules') continue;
      yield* clientTreeFiles(absolute);
    } else if (entry.isFile() && /\.(?:ts|tsx|js|mjs|cjs)$/.test(entry.name)) {
      yield absolute;
    }
  }
}
