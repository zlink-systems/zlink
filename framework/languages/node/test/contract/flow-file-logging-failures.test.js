const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const test = require('node:test');
const ts = require('typescript');

function loadLogging(sample, fileSystem) {
  let exporter;
  const stderr = [];
  const module = { exports: {} };
  const mocks = {
    'node:fs': fileSystem,
    'node:path': path,
    '@opentelemetry/api-logs': { logs: { setGlobalLoggerProvider() {} } },
    '@opentelemetry/sdk-logs': {
      LoggerProvider: class {},
      SimpleLogRecordProcessor: class {
        constructor(options) {
          exporter = options.exporter;
        }
      }
    },
    '@opentelemetry/core': { ExportResultCode: { SUCCESS: 0, FAILED: 1 } }
  };
  const filename = path.resolve(__dirname, '../../samples', sample, 'Server/flow-file-logging.ts');
  const output = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, esModuleInterop: true }
  }).outputText;
  vm.runInNewContext(
    output,
    {
      module,
      exports: module.exports,
      require(name) {
        assert.ok(Object.hasOwn(mocks, name), name);
        return mocks[name];
      },
      process: {
        argv: ['node', 'server', '--config', 'node-wire-config.json'],
        stderr: {
          write(line) {
            stderr.push(line);
          }
        }
      }
    },
    { filename }
  );
  return { enable: module.exports.enableFlowFileLogging, stderr, exporter: () => exporter };
}

for (const sample of ['SupportChat.Ts', 'TicTacToe.Ts', 'Bingo.Ts']) {
  test(`${sample} flow configuration failure reaches stderr without stopping startup`, () => {
    const cause = new Error('node-wire configuration read failed');
    const logging = loadLogging(sample, {
      readFileSync() {
        throw cause;
      }
    });
    assert.doesNotThrow(() => logging.enable('server'));
    assert.match(logging.stderr.join(''), /node-wire configuration read failed/);
  });

  test(`${sample} flow exporter reports file failure and successful export separately`, () => {
    const cause = new Error('node-wire flow append failed');
    let appendError = cause;
    const logging = loadLogging(sample, {
      readFileSync() {
        return JSON.stringify({ sample: { logDir: 'node-wire-flow' } });
      },
      mkdirSync() {},
      appendFile(_file, _lines, callback) {
        callback(appendError);
      }
    });
    logging.enable('server');
    const results = [];
    logging.exporter().export([], (result) => results.push(result));
    assert.equal(results[0].code, 1);
    assert.equal(results[0].error, cause);
    appendError = null;
    logging.exporter().export([], (result) => results.push(result));
    assert.equal(results[1].code, 0);
    assert.equal(logging.stderr.length, 0);
  });
}
