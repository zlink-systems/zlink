const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const source = fs.readFileSync(path.join(__dirname, '../Server/ZoneNode/main.ts'), 'utf8');
const start = source.indexOf('async function waitForBotStart(');
const end = source.indexOf('async function waitForPlacementPeer(', start);
assert.ok(start >= 0 && end > start);
const compiled = ts.transpileModule(
  `${source.slice(start, end)}\nexports.waitForBotStart = waitForBotStart;`,
  { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.CommonJS } }
).outputText;

test('a pending bot-start wait releases its timer on shutdown', async () => {
  const module = {};
  let signalUsed = false;
  const fakeFs = { existsSync: () => false };
  const fakeDelay = (_duration, _value, options) => {
    if (options?.signal === undefined) return new Promise(() => {});
    signalUsed = true;
    return new Promise((_resolve, reject) => {
      options.signal.addEventListener('abort', () =>
        reject(new DOMException('aborted', 'AbortError'))
      );
    });
  };
  new Function('exports', 'fs', 'delay', compiled)(module, fakeFs, fakeDelay);
  const controller = new AbortController();
  let settled = false;
  const pending = module
    .waitForBotStart('/not-created/bots.start', controller.signal)
    .catch((error) => {
      assert.equal(error.name, 'AbortError');
      settled = true;
    });
  await new Promise((resolve) => setImmediate(resolve));
  controller.abort();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(signalUsed, true);
  assert.equal(settled, true);
  await pending;
});

test('shutdown cannot release bot ticks when the file already exists', async () => {
  const module = {};
  new Function('exports', 'fs', 'delay', compiled)(module, { existsSync: () => true }, () =>
    assert.fail('no delay expected')
  );
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(module.waitForBotStart('/created/bots.start', controller.signal), {
    name: 'AbortError'
  });
});
