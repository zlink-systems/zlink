#!/usr/bin/env node
// SPDX-License-Identifier: MPL-2.0

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const ts = require('typescript');

const root = path.resolve(__dirname, '..');
const configPath = path.join(root, 'tsconfig.json');
const config = ts.readConfigFile(configPath, ts.sys.readFile);
if (config.error) {
  throw new Error(ts.flattenDiagnosticMessageText(config.error.messageText, '\n'));
}
const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, root);
const program = ts.createProgram(parsed.fileNames, parsed.options);
const source = program.getSourceFile(path.join(root, 'src', 'index.ts'));
const checker = program.getTypeChecker();
const moduleSymbol = source ? checker.getSymbolAtLocation(source) : undefined;
if (!source || !moduleSymbol) {
  throw new Error('Unable to inspect the public TypeScript entry point');
}
const names = checker.getExportsOfModule(moduleSymbol)
  .filter((symbol) => (symbol.flags & ts.SymbolFlags.Value) !== 0)
  .map((symbol) => symbol.getName())
  .filter((name) => /^[$A-Z_a-z][$\w]*$/.test(name) && name !== 'default')
  .sort((left, right) => left.localeCompare(right, 'en'));

const esm = [
  '// Generated from src/index.ts by scripts/generate_esm_wrapper.js.',
  "import cjs from './index.js';",
  'export default cjs;',
  `export const { ${names.join(', ')} } = cjs;`,
  ''
].join('\n');
const declarations = [
  "export * from './index.js';",
  "import cjs = require('./index.js');",
  'export default cjs;',
  ''
].join('\n');

fs.writeFileSync(path.join(root, 'dist', 'index.mjs'), esm);
fs.writeFileSync(path.join(root, 'dist', 'index.d.mts'), declarations);
