'use strict';

const { build: compile } = require('esbuild');
const assert = require('node:assert/strict');
const { mkdtempSync, mkdirSync, readFileSync, writeFileSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join, resolve, delimiter } = require('node:path');
const { spawnSync } = require('node:child_process');
const ts = require('typescript');

(async () => {
const root = resolve(__dirname, '..');
const runDir = process.env.ZLINK_TELEMETRY_RUN_DIR || mkdtempSync(join(tmpdir(), 'zlink-telemetry-'));
mkdirSync(runDir, { recursive: true });
const modules = ['message-flow', 'message-flow-delivery', 'message-flow-record',
  'message-flow-worker', 'message-flow-bootstrap', 'flow-context'];
const outdir = join(runDir, 'modules');
const build = await compile({
  entryPoints: Object.fromEntries([
    ...modules.map(name => [name, join(root, 'packages/framework/src/runtime/diagnostics', `${name}.ts`)]),
    ['dispatch-error-reporter', join(root, 'packages/framework/src/runtime/channels/dispatch-error-reporter.ts')],
    ['channel-envelope', join(root, 'packages/framework/src/runtime/channels/channel-envelope.ts')]
  ]),
  bundle: true,
  platform: 'node',
  format: 'cjs',
  packages: 'external',
  external: modules.map(name => `./${name}`),
  plugins: [{ name: 'native-free-source-boundary', setup(builder) {
    // Drop only unused native imports. Any referenced native symbol remains an
    // import and the guard below rejects the proof before code is executed.
    builder.onResolve({ filter: /^@zlink-systems\/zlink(?:\/|$)/ }, args =>
      ({ path: args.path, external: true, sideEffects: false }));
    // The flat proof output must share the same ambient-flow module that the
    // normal directory-preserving TypeScript build shares.
    builder.onResolve({ filter: /(?:^|\/)flow-context$/ }, () =>
      ({ path: './flow-context', external: true }));
  } }],
  metafile: true,
  outdir,
  logLevel: 'warning'
});
const imports = Object.fromEntries(Object.entries(build.metafile.outputs).map(([name, output]) => [name, output.imports]));
writeFileSync(join(runDir, 'source-module-imports.json'), JSON.stringify(imports, null, 2));
assert.ok(Object.values(imports).flat().every(value => !value.path.startsWith('@zlink-systems/zlink')),
  'native reference remains in the source proof; refusing to load the SDK cache');
// Reuse the existing provider/payload/level/sampling assertions verbatim. The
// remaining tests in that file need native host/transport, so are not selected.
const contractPath = join(root, 'test/contract/message-flow.test.js');
const source = ts.createSourceFile(contractPath, readFileSync(contractPath, 'utf8'), ts.ScriptTarget.Latest, true, ts.ScriptKind.JS);
const helpers = new Set(['diagnostics', 'makeTracer', 'receivedEvent', 'assertFlowPublication']);
const values = new Set(['ZLinkMessageFlowOutcome', 'ZLinkDispatchErrorSurface', 'ZLinkDispatchMessageKind']);
const selected = [];
let foundation = true;
for (const statement of source.statements) {
  if (ts.isFunctionDeclaration(statement) && helpers.has(statement.name?.text)) selected.push(statement.getText(source));
  else if (ts.isVariableStatement(statement) && statement.declarationList.declarations.some(value => values.has(value.name.getText(source)))) selected.push(statement.getText(source));
  else if (ts.isExpressionStatement(statement) && ts.isCallExpression(statement.expression)
    && statement.expression.expression.getText(source) === 'test') {
    const title = statement.expression.arguments[0];
    if (ts.isStringLiteral(title) && title.text.startsWith('channel request completion owner')) foundation = false;
    if (ts.isStringLiteral(title) && ((foundation && title.text !== 'message-flow async and sync controls complete before observation')
      || title.text === 'MFLOW-EXT channel wire and outbound trace use the same created flow')) selected.push(statement.getText(source));
  }
}
const selectedPath = join(runDir, 'existing-message-flow-contracts.cjs');
writeFileSync(selectedPath, `const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMessageFlowTracer, createDiagnosticsContext, createMessageFlowModeCell } = require(${JSON.stringify(join(outdir, 'message-flow.js'))});
const { ZLinkDispatchErrorReporter } = require(${JSON.stringify(join(outdir, 'dispatch-error-reporter.js'))});
const flowContext = require(${JSON.stringify(join(outdir, 'flow-context.js'))});
const channelEnvelope = require(${JSON.stringify(join(outdir, 'channel-envelope.js'))});
const delivery = require(${JSON.stringify(join(outdir, 'message-flow-delivery.js'))});
const observation = require(${JSON.stringify(join(root, 'test/contract/fixtures/message-flow-observation.cjs'))})(delivery);
const { telemetryRecords, traceRecords } = observation;
const observeTelemetry = () => observation.observed();
test.beforeEach(async () => observation.reset());
test.after(() => observation.close());
${selected.join('\n\n')}
`);
const result = spawnSync(process.execPath, ['--test', join(root, 'test/contract/message-flow-delivery.test.js'), selectedPath], {
  cwd: root,
  env: { ...process.env,
    NODE_PATH: [join(root, 'node_modules'), process.env.NODE_PATH].filter(Boolean).join(delimiter),
    ZLINK_TEST_TELEMETRY_MODULE_DIR: outdir, ZLINK_TELEMETRY_RUN_DIR: runDir },
  stdio: 'inherit'
});
console.log(`Telemetry source proof artifacts: ${runDir}`);
process.exitCode = result.status ?? 1;

})().catch(error => { console.error(error); process.exitCode = 1; });
