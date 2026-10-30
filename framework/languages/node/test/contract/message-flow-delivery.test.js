'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { mkdtempSync, writeFileSync, mkdirSync, readFileSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join, resolve } = require('node:path');
const { pathToFileURL } = require('node:url');
const ts = require('typescript');

const root = resolve(__dirname, '../..');
const runDir = process.env.ZLINK_TELEMETRY_RUN_DIR || mkdtempSync(join(tmpdir(), 'zlink-telemetry-contract-'));
mkdirSync(runDir, { recursive: true });
const moduleDir = process.env.ZLINK_TEST_TELEMETRY_MODULE_DIR || join(root, 'packages/framework/dist/runtime/diagnostics');
const fixture = join(__dirname, 'fixtures/telemetry-worker-preload.cjs');
const scenario = join(__dirname, 'fixtures/telemetry-worker-case.cjs');

// Execute the real stop method without loading the unavailable native package.
// Only its lifecycle collaborators are supplied by the test; the method body,
// timeout constant and Framework exception definitions come from source.
const hostPath = join(root, 'packages/framework/src/runtime/host/index.ts');
const hostSource = ts.createSourceFile(hostPath, readFileSync(hostPath, 'utf8'), ts.ScriptTarget.Latest, true);
const hostClass = hostSource.statements.find(node => ts.isClassDeclaration(node) && node.name?.text === 'ZLinkFrameworkRuntimeHost');
const stopMethod = hostClass.members.find(node => node.name?.getText(hostSource) === 'stop');
const timeoutConstant = hostSource.statements.find(node => ts.isVariableStatement(node)
  && node.declarationList.declarations.some(value => value.name.getText(hostSource) === 'DEFAULT_HOST_CONTROL_TIMEOUT_MS'));
const errorsPath = join(root, 'packages/framework/src/contracts/Errors/ZLinkFrameworkException.ts');
const errorsOutput = join(runDir, 'host-stop-errors.cjs');
const compilerOptions = { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS };
writeFileSync(errorsOutput, ts.transpileModule(readFileSync(errorsPath, 'utf8'), { compilerOptions }).outputText);
const stopOutput = join(runDir, 'host-stop-source.cjs');
writeFileSync(stopOutput, ts.transpileModule(`
const { ZLinkFrameworkException, ZLinkFrameworkErrorKind } = require(${JSON.stringify(errorsOutput)});
const { retireTelemetryLease } = require(${JSON.stringify(join(moduleDir, 'message-flow-delivery.js'))});
${timeoutConstant.getText(hostSource)}
class HostStopSourceProof { ${stopMethod.getText(hostSource)} }
module.exports = HostStopSourceProof.prototype.stop;
`, { compilerOptions }).outputText);

for (const mode of ['bootstrap', 'none', 'main-only', 'logger', 'trace', 'both', 'false-enabled', 'registered-noop',
  'error-filter', 'blocked', 'blocked-trace', 'overflow-width', 'failures', 'fallback-failure', 'logger-init-failure', 'trace-init-failure', 'lease-renewal',
  'throwing-preload', 'worker-start-failure', 'worker-exit', 'role-counter-width', 'common-helper']) {
  test(`message-flow process delivery: ${mode}`, () => {
    const result = spawnSync(process.execPath, ['--require', fixture, scenario], {
      cwd: root,
      timeout: 25000,
      env: {
        ...process.env,
        ZLINK_TEST_TELEMETRY_MODULE_DIR: moduleDir,
        ZLINK_TEST_TELEMETRY_HOST_STOP: stopOutput,
        ZLINK_TEST_TELEMETRY_CASE: mode,
        ZLINK_TEST_TELEMETRY_RECORDS: join(runDir, `${mode}.jsonl`),
        ZLINK_TEST_TELEMETRY_GATE: join(runDir, `${mode}.release`)
      }, encoding: 'utf8'
    });
    writeFileSync(join(runDir, `${mode}.log`), result.stdout + result.stderr);
    assert.equal(result.error, undefined, result.error?.message);
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, new RegExp(`PASS ${mode}`));
  });
}

for (const configuration of ['command-import', 'environment-require', 'mixed-preloads']) {
  test(`message-flow standard preload inheritance: ${configuration}`, () => {
    assert.ok(process.allowedNodeEnvironmentFlags.has('--require'));
    const options = value => `"${value.replace(/\\/g, '/')}"`;
    const args = configuration === 'command-import' ? ['--import', pathToFileURL(fixture).href]
      : configuration === 'mixed-preloads' ? ['--require', join(__dirname, 'fixtures/telemetry-order-second.cjs'),
        '--import', pathToFileURL(join(__dirname, 'fixtures/telemetry-order-second.mjs')).href] : [];
    const inherited = process.env.NODE_OPTIONS || '';
    const nodeOptions = configuration === 'environment-require' ? `${inherited} --require ${options(fixture)}`
      : configuration === 'mixed-preloads' ? `${inherited} --require ${options(join(__dirname, 'fixtures/telemetry-order-first.cjs'))}`
        + ` --import ${options(pathToFileURL(join(__dirname, 'fixtures/telemetry-order-first.mjs')).href)}` : inherited;
    const result = spawnSync(process.execPath, [...args, scenario], {
      cwd: root, timeout: 25000, encoding: 'utf8',
      env: { ...process.env, NODE_OPTIONS: nodeOptions,
        ZLINK_TEST_TELEMETRY_MODULE_DIR: moduleDir,
        ZLINK_TEST_TELEMETRY_HOST_STOP: stopOutput,
        ZLINK_TEST_TELEMETRY_CASE: 'logger',
        ZLINK_TEST_TELEMETRY_ORDER: configuration === 'mixed-preloads' ? '1' : '0',
        ZLINK_TEST_TELEMETRY_RECORDS: join(runDir, `${configuration}.jsonl`),
        ZLINK_TEST_TELEMETRY_GATE: join(runDir, `${configuration}.release`) }
    });
    writeFileSync(join(runDir, `${configuration}.log`), result.stdout + result.stderr);
    assert.equal(result.error, undefined, result.error?.message);
    assert.equal(result.status, 0, result.stdout + result.stderr);
  });
}
