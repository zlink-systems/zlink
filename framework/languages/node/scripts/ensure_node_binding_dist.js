const fs = require('node:fs');
const path = require('node:path');

const nodeRoot = path.resolve(__dirname, '..');
const bindingPackageRoot = path.join(nodeRoot, 'node_modules', '@zlink-systems', 'zlink');

function ensureNodeBindingPackage() {
  if (hasRequiredPackage()) {
    return;
  }
  throw new Error(
    'Node binding package is missing. Run npm install in framework/languages/node after building the local package.'
  );
}

function hasRequiredPackage() {
  return [
    path.join(bindingPackageRoot, 'dist', 'index.js'),
    path.join(bindingPackageRoot, 'dist', 'index.d.ts')
  ].every((file) => fs.existsSync(file));
}

module.exports = { ensureNodeBindingPackage };
