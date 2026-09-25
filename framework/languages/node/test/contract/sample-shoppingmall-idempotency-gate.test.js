const assert = require('node:assert/strict');
const fs = require('node:fs');
const Module = require('node:module');
const path = require('node:path');
const test = require('node:test');
const ts = require('typescript');

const root = path.resolve(__dirname, '../..');
const useCasePath = path.join(
  root,
  'samples/ShoppingMall.Ts/Server/CommerceApi/Application/start-order-use-case.ts'
);

function loadUseCase() {
  const source = fs.readFileSync(useCasePath, 'utf8');
  const compiled = ts.transpileModule(source, {
    compilerOptions: {
      module: ts.ModuleKind.CommonJS,
      target: ts.ScriptTarget.ES2022,
      experimentalDecorators: true,
      emitDecoratorMetadata: false
    }
  }).outputText;
  const loaded = new Module(useCasePath, module);
  const framework = {
    ZLinkFrameworkErrorKind: { Rejected: 4 },
    ZLinkFrameworkException: class ZLinkFrameworkException extends Error {
      constructor(kind) {
        super('Framework rejected the request.');
        this.kind = kind;
      }
    }
  };
  loaded.require = (specifier) => {
    if (specifier === '@nestjs/common') return { Injectable: () => (type) => type };
    if (specifier === '@zlink-systems/framework') return framework;
    throw new Error(`Unexpected sample dependency: ${specifier}`);
  };
  loaded._compile(compiled, useCasePath);
  return { StartOrderUseCase: loaded.exports.StartOrderUseCase, framework };
}

test('CommerceApi returns stored order state after a rejected start', async () => {
  const { StartOrderUseCase, framework } = loadUseCase();
  const request = { idempotencyKey: 'shared-key' };
  const state = { orderId: 'order-shared-key', status: 'Created' };
  const calls = [];
  const store = {
    reserveOrder(value) {
      calls.push('reserve');
      return value;
    },
    getOrderByIdempotencyKey(key) {
      calls.push(`lookup:${key}`);
      return state;
    }
  };
  const router = {
    async start() {
      calls.push('start');
      throw new framework.ZLinkFrameworkException(framework.ZLinkFrameworkErrorKind.Rejected);
    }
  };

  assert.deepEqual(await new StartOrderUseCase(router, store).start(request), { state });
  assert.deepEqual(calls, ['reserve', 'start', 'lookup:shared-key']);
});

test('CommerceApi does not claim success without a stored order', async () => {
  const { StartOrderUseCase, framework } = loadUseCase();
  const rejected = new framework.ZLinkFrameworkException(
    framework.ZLinkFrameworkErrorKind.Rejected
  );
  const store = {
    reserveOrder: (request) => request,
    getOrderByIdempotencyKey: () => undefined
  };
  const router = {
    start: async () => {
      throw rejected;
    }
  };

  await assert.rejects(
    new StartOrderUseCase(router, store).start({ idempotencyKey: 'missing' }),
    (error) => error === rejected
  );
});
