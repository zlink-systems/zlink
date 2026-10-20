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

test('channel request and reply encode allocate only the final parts representation', () => {
  const { codec, counts } = countedCodec('channels/channel-envelope');
  const codecs = { serializers: new Map() };
  const data = { body: 'x'.repeat(1024) };
  for (let index = 0; index < 1000; index++) {
    const parts = codec.encodeChannelEnvelopePartsAtDeadline(
      1, 'codec', 'Packet', data, 2000000000000, undefined, codecs,
      '00112233445566778899aabbccddeeff', false
    );
    const request = codec.decodeChannelHeader(parts.map(part => ZLinkBufferMessage.fromOwned(part)), false);
    const reply = codec.encodeChannelReplyParts(request, data, codecs);
    const response = codec.decodeChannelEnvelope(reply.map(part => ZLinkBufferMessage.fromOwned(part)), undefined, false);
    assert.deepEqual(codec.decodeChannelPayload(response), data);
  }
  assert.equal(counts.get('encodePayload:object:contentType,message') ?? 0, 0);
});

test('correlation validation accepts the full ASCII contract without per-call RegExp objects', () => {
  const { codec, counts } = countedCodec('channels/channel-envelope');
  for (let index = 0; index < 1000; index++) {
    assert.equal(codec.isValidChannelCorrelationId('a'.repeat(64)), true);
    assert.equal(codec.isValidChannelCorrelationId('\0\x7f'), true);
    for (const value of ['', 'a'.repeat(65), 'é', '\ud800']) {
      assert.equal(codec.isValidChannelCorrelationId(value), false);
    }
  }
  assert.equal(counts.get('isValidChannelCorrelationId:regexp') ?? 0, 0);
});

async function trackedCodecAllocations(filename, action) {
  const inspector = require('node:inspector');
  const session = new inspector.Session();
  session.connect();
  const post = (method, params = {}) => new Promise((resolve, reject) => {
    session.post(method, params, (error, result) => error ? reject(error) : resolve(result));
  });
  const chunks = [];
  try {
    await post('HeapProfiler.enable');
    await post('HeapProfiler.startTrackingHeapObjects', { trackAllocations: true });
    for (let index = 0; index < 1000; index++) action();
    session.on('HeapProfiler.addHeapSnapshotChunk', ({ params }) => chunks.push(params.chunk));
    await post('HeapProfiler.stopTrackingHeapObjects', { reportProgress: false });
  } finally {
    session.disconnect();
  }
  const snapshot = JSON.parse(chunks.join(''));
  const fields = snapshot.snapshot.meta.trace_function_info_fields;
  const infos = snapshot.trace_function_infos;
  const allocations = new Map();
  function visit(nodes, owner) {
    for (let index = 0; index < nodes.length; index += 5) {
      const offset = nodes[index + 1] * fields.length;
      const name = snapshot.strings[infos[offset + fields.indexOf('name')]];
      const script = snapshot.strings[infos[offset + fields.indexOf('script_name')]];
      const next = script.endsWith(filename) ? name : owner;
      if (next !== undefined) {
        const entry = allocations.get(next) ?? { objects: 0, bytes: 0 };
        entry.objects += nodes[index + 2];
        entry.bytes += nodes[index + 3];
        allocations.set(next, entry);
      }
      visit(nodes[index + 4], next);
    }
  }
  visit(snapshot.trace_tree);
  return allocations;
}

test('warmed serializer lookup does not allocate preparation closure contexts', async () => {
  const { codec } = countedCodec('messaging/payload-codec');
  class Payload {}
  const payload = new Payload();
  const serializers = new Map([['application/x-test', {}]]);
  const lookup = () => codec.selectSerializerWithContentType(payload, serializers);
  for (let index = 0; index < 1000; index++) lookup();
  const allocations = await trackedCodecAllocations('/runtime/messaging/payload-codec.js', lookup);
  assert.equal(allocations.get('serializerSelectionPlanOf')?.objects ?? 0, 0);
});
