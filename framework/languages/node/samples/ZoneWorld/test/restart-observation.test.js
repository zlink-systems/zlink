const assert = require('node:assert/strict');
const fs = require('node:fs');
const Module = require('node:module');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');
const stream = require('../../../packages/stream-connector/dist');
const wire = require('@zlink-systems/stream-wire');
const { ZoneWorldErrors } = require('../dist/Shared/spec');

const sampleRoot = path.resolve(__dirname, '..');
const target = 'zone-node-2';
const turn = () => new Promise((resolve) => setImmediate(resolve));

function frame(kind, name, payload, requestSeq) {
  return wire.encodeStreamWireFrame(
    wire.encodeStreamWireHeader({
      kind,
      codec: wire.ZlinkStreamCodec.Json,
      flags: requestSeq === undefined ? 0 : wire.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq,
      name,
      metadata: new Map()
    }),
    Buffer.from(JSON.stringify(payload))
  );
}

async function fixture(diagnostics) {
  const inbound = [];
  const requests = [];
  let replacementReady = false;
  let instance;
  let reader;
  let closed = false;
  function push(bytes) {
    if (reader === undefined) inbound.push(bytes);
    else {
      const resolve = reader;
      reader = undefined;
      resolve(bytes);
    }
  }
  const connection = {
    async read() {
      if (inbound.length > 0) return inbound.shift();
      if (closed) return undefined;
      return new Promise((resolve) => {
        reader = resolve;
      });
    },
    async close() {
      closed = true;
      if (reader !== undefined) push(undefined);
    },
    async write(bytes) {
      const decoded = wire.decodeStreamWireFrame(bytes);
      const header = wire.decodeStreamWireHeader(decoded.header);
      requests.push(header.name);
      const payload = JSON.parse(Buffer.from(decoded.payload).toString());
      if (header.name === 'NodeDiagnosticsReq' && replacementReady) {
        push(
          frame(
            wire.ZlinkStreamMessageKind.Response,
            'NodeDiagnosticsRes',
            { zones: [], playerCount: 0, ...diagnostics },
            header.requestSeq
          )
        );
      } else if (header.name === 'SetMaintenanceReq') {
        push(
          frame(
            wire.ZlinkStreamMessageKind.Response,
            'SetMaintenanceRes',
            {
              nodeId: target,
              enabled: payload.enabled,
              zones: [],
              error: null
            },
            header.requestSeq
          )
        );
        push(
          frame(wire.ZlinkStreamMessageKind.Send, 'NodeStatusNotify', {
            nodeId: target,
            registered: true,
            connected: true,
            maintenance: false,
            zones: [],
            playerCount: 0
          })
        );
      }
    }
  };
  // Execute the production scenario without its CLI entry point. Only the public
  // transport factory is replaced; queue selection and waits use the real connector.
  const filename = path.join(sampleRoot, 'dist/Client/special.js');
  const scenario = new Module(filename, module);
  scenario.filename = filename;
  scenario.paths = Module._nodeModulePaths(path.dirname(filename));
  const originalRequire = scenario.require.bind(scenario);
  scenario.require = (name) =>
    name === './join-readiness'
      ? {}
      : name === '@zlink-systems/stream-connector'
        ? {
            ...stream,
            zlinkStreamConnectorFactory: {
              create(options) {
                instance = stream.zlinkStreamConnectorFactory.create({
                  ...options,
                  transportFactory: {
                    async connect() {
                      return connection;
                    }
                  }
                });
                return instance;
              }
            }
          }
        : originalRequire(name);
  const code = ts.transpileModule(
    fs.readFileSync(path.join(sampleRoot, 'Client/special.ts'), 'utf8'),
    {
      compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
    }
  ).outputText;
  scenario._compile(
    code.slice(0, code.lastIndexOf('main().catch(')) +
      '\nexports.restore = runMaintenanceRestore;\n',
    filename
  );
  let failure;
  const completed = scenario.exports.restore('ws://127.0.0.1:19000', target).catch((error) => {
    failure = error;
  });
  await turn();
  return {
    requests,
    instance,
    async status(payload, isReplacement = false) {
      replacementReady = isReplacement;
      push(
        frame(wire.ZlinkStreamMessageKind.Send, 'NodeStatusNotify', {
          nodeId: target,
          registered: true,
          connected: true,
          maintenance: true,
          zones: [],
          playerCount: 0,
          ...payload
        })
      );
      await instance.dispatch();
      await turn();
    },
    async finish() {
      await completed;
      return failure;
    },
    async close() {
      await instance.close();
      await completed;
    }
  };
}

for (const diagnostics of [
  { nodeId: target, maintenance: true, error: null },
  { nodeId: target, maintenance: false, error: null },
  { nodeId: target, maintenance: true, error: ZoneWorldErrors.nodeUnavailable }
]) {
  test(`E5 consumes old ready before stopped and preserves diagnostics assertion: ${JSON.stringify(diagnostics)}`, async () => {
    const server = await fixture(diagnostics);
    try {
      await server.status({});
      assert.equal(server.instance.receivedCount('NodeStatusNotify'), 1);
      await server.status({ connected: false });
      await server.status({ nodeId: 'zone-node-1' });
      assert.equal(
        server.requests.filter((name) => name === 'NodeDiagnosticsReq').length,
        0,
        'diagnostics must remain pending until replacement ready, even with old true queued'
      );
      await server.status({ registered: false });
      assert.equal(server.requests.filter((name) => name === 'NodeDiagnosticsReq').length, 0);
      await server.status({}, true);
      const failure = await server.finish();
      assert.equal(server.requests.filter((name) => name === 'NodeDiagnosticsReq').length, 1);
      if (diagnostics.error === null && diagnostics.maintenance) assert.equal(failure, undefined);
      else assert.match(failure.message, /maintenance state was not restored|could not reach/);
    } finally {
      await server.close();
    }
  });
}

test('E5 does not treat report TTL expiry as the old incarnation stopping', async () => {
  const server = await fixture({ nodeId: target, maintenance: true, error: null });
  try {
    await server.status({ registered: false });
    await server.status({});
    assert.equal(server.requests.filter((name) => name === 'NodeDiagnosticsReq').length, 0);
    await server.status({ connected: false });
    await server.status({}, true);
    assert.equal(await server.finish(), undefined);
    assert.equal(server.requests.filter((name) => name === 'NodeDiagnosticsReq').length, 1);
  } finally {
    await server.close();
  }
});
