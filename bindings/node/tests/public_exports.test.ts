'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const zlink = require('@zlink-systems/zlink');
const ts = require('typescript');
const path = require('node:path');

test('emitted declarations preserve every public package export', () => {
  const root = path.resolve(__dirname, '../..');
  function exportsAt(entry) {
    const program = ts.createProgram([entry], {
      typeRoots: [path.join(root, 'node_modules/@types')],
      types: ['node'],
      skipLibCheck: false
    });
    const checker = program.getTypeChecker();
    return checker.getExportsOfModule(checker.getSymbolAtLocation(program.getSourceFile(entry)))
      .filter((symbol) => {
        const target = symbol.flags & ts.SymbolFlags.Alias
          ? checker.getAliasedSymbol(symbol) : symbol;
        return (target.getDeclarations()?.length ?? 0) > 0;
      }).map((symbol) => symbol.name);
  }
  const declarations = new Set(exportsAt(path.join(root, 'dist/index.d.ts')));
  const missing = exportsAt(path.join(root, 'src/index.ts'))
    .filter((name) => !declarations.has(name));
  assert.deepEqual(missing, []);
  assert.ok(declarations.has('MessageLike'));
});

test('TypeScript consumers validate the complete declaration graph', () => {
  const root = path.resolve(__dirname, '../..');
  const program = ts.createProgram([path.join(root, 'dist/index.d.ts')], {
    target: ts.ScriptTarget.ES2022,
    module: ts.ModuleKind.NodeNext,
    moduleResolution: ts.ModuleResolutionKind.NodeNext,
    strict: true,
    noEmit: true,
    skipLibCheck: false,
    typeRoots: [path.join(root, 'node_modules/@types')],
    types: ['node']
  });
  assert.deepEqual(ts.getPreEmitDiagnostics(program).map((diagnostic) =>
    ts.flattenDiagnosticMessageText(diagnostic.messageText, '\n')), []);
});

test('public root exports raw capabilities without service projections', () => {
  for (const name of [
    'createContext',
    'createPairSocket',
    'createRouterSocket',
    'createStreamSocket',
    'createPoller',
    'createTimer',
    'Received',
    'Message',
    'ReplyToken',
    'StreamPacket',
    'StreamRecvMode',
    'CompletionKind'
  ]) {
    assert.notEqual(zlink[name], undefined, name);
  }
});

test('ESM wrapper preserves runtime contract values exported from the CJS barrel', async () => {
  const esm = await import('../dist/index.mjs');
  for (const name of [
    'Message',
    'Received',
    'TopicMessage',
    'SubscriptionEvent',
    'CompletionKind'
  ]) {
    assert.equal(typeof esm[name], name === 'CompletionKind' ? 'object' : 'function', name);
  }
});

test('package exports block internal addon and runtime modules', () => {
  assert.throws(
    () => require('@zlink-systems/zlink/dist/zlink/runtime/native/native'),
    { code: 'ERR_PACKAGE_PATH_NOT_EXPORTED' }
  );
});
