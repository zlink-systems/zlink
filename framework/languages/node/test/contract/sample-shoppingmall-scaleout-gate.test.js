const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('ShoppingMall starts scale-out orders concurrently', () => {
  const client = fs.readFileSync(path.join(
    root,
    'samples/ShoppingMall.Ts/Client/shoppingmall-client-scenario.ts'
  ), 'utf8');
  const scaleOut = client.match(/const \[scaleA, scaleB\] = await Promise\.all\(\[[\s\S]*?\]\);/);

  assert.ok(scaleOut, 'ShoppingMall scale-out start requests are not submitted together.');
  assert.match(scaleOut[0], /order-scale-001/);
  assert.match(scaleOut[0], /order-scale-002/);
});
