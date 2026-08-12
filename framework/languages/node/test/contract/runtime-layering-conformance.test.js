'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const sourceRoot = path.resolve(
  __dirname,
  '../../packages/framework/src/runtime'
);

function typeScriptFiles(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
    const target = path.join(directory, entry.name);
    return entry.isDirectory()
      ? typeScriptFiles(target)
      : entry.isFile() && entry.name.endsWith('.ts') ? [target] : [];
  });
}

test('domain owners depend on raw backend ports rather than the Node binding adapter', () => {
  const domainDirectories = ['foundation', 'actors', 'spots', 'locations'];
  const violations = domainDirectories.flatMap(directory =>
    typeScriptFiles(path.join(sourceRoot, directory)).flatMap(file => {
      const source = fs.readFileSync(file, 'utf8');
      return /from ['"][^'"]*backend\/node\//.test(source)
        ? [path.relative(sourceRoot, file)]
        : [];
    })
  );
  assert.deepEqual(violations, []);

  const useCase = fs.readFileSync(
    path.join(sourceRoot, 'foundation/raw-service-mesh-runtime.ts'),
    'utf8'
  );
  assert.match(useCase, /from '\.\.\/backend\/raw-binding-port'/);
  assert.doesNotMatch(useCase, /ZLinkNodeRawBindingPort/);

  const adapter = fs.readFileSync(
    path.join(sourceRoot, 'backend/node/node-raw-binding-port.ts'),
    'utf8'
  );
  assert.match(adapter, /implements ZLinkRawBindingPort/);
  assert.match(adapter, /from '\.\.\/raw-binding-port'/);
});
