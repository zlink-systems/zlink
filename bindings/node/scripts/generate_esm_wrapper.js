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
  // `export *` exposes declarations from the contracts barrel as aliases.
  // Check the aliased declaration as well; otherwise runtime classes such as
  // Message and Received disappear from the ESM wrapper even though the
  // CommonJS entry point exports them.
  .filter((symbol) => {
    // A named `export type { ... }` is represented by the same aliased
    // declaration as its runtime counterpart. Keep the type-only alias out
    // of the value destructuring even when the target also has a value.
    if ((symbol.declarations ?? []).some((declaration) => {
      let current = declaration;
      while (current) {
        if (ts.isExportDeclaration(current) && current.isTypeOnly) return true;
        current = current.parent;
      }
      return false;
    })) return false;
    const resolved = (symbol.flags & ts.SymbolFlags.Alias) !== 0
      ? checker.getAliasedSymbol(symbol)
      : symbol;
    return (resolved.flags & ts.SymbolFlags.Value) !== 0;
  })
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
