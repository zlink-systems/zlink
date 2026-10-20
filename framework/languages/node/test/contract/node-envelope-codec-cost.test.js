'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const Module = require('node:module');
const ts = require('typescript');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');

// Count evaluated allocation expressions in the real module, including objects
// collected before an ordinary heap snapshot. Dependencies use their normal code.
function countedCodec(relativePath) {
  const filename = path.resolve(__dirname, '../../packages/framework/dist/runtime', relativePath + '.js');
  const sourcePath = filename.replace('/dist/', '/src/').replace(/\.js$/, '.ts');
  const source = ts.createSourceFile(sourcePath, fs.readFileSync(sourcePath, 'utf8'), ts.ScriptTarget.Latest, true);
  const counts = new Map();
  const f = ts.factory;
  const transformed = ts.transform(source, [context => {
    let owner;
    function visit(node) {
      const previous = owner;
      if (ts.isFunctionDeclaration(node)) owner = node.name?.getText(source);
      const key = ts.isObjectLiteralExpression(node)
        ? `${owner}:object:${node.properties.map(p => p.name?.getText(source)).join(',')}`
        : ts.isRegularExpressionLiteral(node) ? `${owner}:regexp` : undefined;
      let result = ts.visitEachChild(node, visit, context);
      if (key !== undefined) {
        result = f.createCallExpression(f.createIdentifier('recordAllocation'), undefined, [f.createStringLiteral(key), result]);
      }
      owner = previous;
      return result;
    }
    return node => ts.visitNode(node, visit);
  }]);
  const compiled = ts.transpileModule(ts.createPrinter().printFile(transformed.transformed[0]), {
    compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS, esModuleInterop: true }
  });
  transformed.dispose();
  const loaded = new Module(filename, module);
  loaded.filename = filename;
  loaded.paths = Module._nodeModulePaths(path.dirname(filename));
  loaded.recordAllocation = (key, value) => {
    counts.set(key, (counts.get(key) ?? 0) + 1);
    return value;
  };
  loaded._compile('const recordAllocation = module.recordAllocation;\n' + compiled.outputText, filename);
  counts.clear();
  return { codec: loaded.exports, counts };
}

test('messaging returns the final Message without a discarded content-type descriptor', () => {
  const { codec, counts } = countedCodec('messaging/payload-codec');
  for (let index = 0; index < 1000; index++) {
    const message = codec.encodeFrameworkPayloadMessage({ body: 'payload' });
    assert.equal(codec.frameworkPayloadContentType(message), 'application/json');
    assert.deepEqual(JSON.parse(message.getString()), { body: 'payload' });
    message.close();
  }
  assert.equal(counts.get('encodeFrameworkPayload:object:message,contentType') ?? 0, 0);
  const encoded = codec.encodeFrameworkPayload({ body: 'explicit descriptor' });
  assert.equal(encoded.contentType, 'application/json');
  encoded.message.close();
});

test('JSON decode returns the value without a success or failure carrier', () => {
  const { codec, counts } = countedCodec('messaging/payload-codec');
  const message = ZLinkBufferMessage.fromOwned(Buffer.from('{"body":"payload"}'));
  for (let index = 0; index < 1000; index++) {
    assert.deepEqual(codec.decodeFrameworkPayloadMessage(message), { body: 'payload' });
  }
  assert.equal(counts.get('parseJsonPayload:object:valid,value') ?? 0, 0);
  assert.throws(
    () => codec.decodeFrameworkPayloadMessage(ZLinkBufferMessage.fromOwned(Buffer.from('{'))),
    error => error.message.includes('PayloadDecodeFailed') && error.cause instanceof SyntaxError
  );
  const configurationFailure = new Error('schema metadata unavailable');
  const declaredType = new Proxy(class Payload {}, {
    getOwnPropertyDescriptor() { throw configurationFailure; }
  });
  assert.throws(
    () => codec.decodeFrameworkPayloadMessage(message, undefined, declaredType),
    error => error === configurationFailure
  );
});
