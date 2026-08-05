const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const nodeRoot = path.resolve(__dirname, '../..');
const amountSourcePath = path.join(
  nodeRoot,
  'samples/ShoppingMall.Ts/Shared/Contracts/decimal-amount.ts'
);

test('ShoppingMall decimal amount preserves two fractional digits in the JSON number wire range', () => {
  const source = fs.readFileSync(amountSourcePath, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
  }).outputText;
  const module = { exports: {} };
  Function('module', 'exports', output)(module, module.exports);
  const { DecimalAmount, MAX_DECIMAL_MINOR_UNITS } = module.exports;

  const amount = DecimalAmount.fromMinorUnits(1234567890123456n);
  const wire = JSON.stringify({ amount });
  assert.equal(wire, '{"amount":12345678901234.56}');
  assert.equal(
    DecimalAmount.fromWire(JSON.parse(wire).amount).toMinorUnits(),
    1234567890123456n
  );
  assert.equal(JSON.stringify({ amount: DecimalAmount.fromMinorUnits(12_000n) }), '{"amount":120}');
  assert.equal(
    DecimalAmount.fromWire(Number(MAX_DECIMAL_MINOR_UNITS) / 100).toMinorUnits(),
    MAX_DECIMAL_MINOR_UNITS
  );
});

test('ShoppingMall decimal amount rejects excess scale and unsafe JSON number values', () => {
  const source = fs.readFileSync(amountSourcePath, 'utf8');
  const output = ts.transpileModule(source, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
  }).outputText;
  const module = { exports: {} };
  Function('module', 'exports', output)(module, module.exports);
  const { DecimalAmount, MAX_DECIMAL_MINOR_UNITS } = module.exports;

  assert.throws(() => DecimalAmount.fromWire(1.001), /at most two fractional digits/);
  assert.throws(
    () => DecimalAmount.fromMinorUnits(MAX_DECIMAL_MINOR_UNITS + 1n),
    /supported JSON number range/
  );
});

test('ShoppingMall wire contracts do not expose amount as an unbounded JavaScript number', () => {
  const contracts = fs.readFileSync(path.join(
    nodeRoot,
    'samples/ShoppingMall.Ts/Shared/Contracts/messages.ts'
  ), 'utf8');

  assert.doesNotMatch(contracts, /readonly amount: number/);
  assert.doesNotMatch(contracts, /amount\?: number/);
  assert.match(contracts, /readonly amount: DecimalAmount/);
  assert.match(contracts, /amount\?: DecimalAmount/);
});
