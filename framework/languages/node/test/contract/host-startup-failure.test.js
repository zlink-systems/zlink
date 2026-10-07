const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');

function hostOptions(store) {
  const options = nestjs.zlinkFramework().addLocationStore(store);
  options
    .addRouteMesh('startup-conflict')
    .listen('tcp://127.0.0.1:*')
    .routingId('startup-conflict');
  return options.build();
}

test('Nest context rejects runtime startup RoutingId conflict', async () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  function moduleForHost() {
    class HostModule {}
    Module({ imports: [nestjs.ZLinkModule.forRoot(hostOptions(store))] })(HostModule);
    return HostModule;
  }
  const owner = await NestFactory.createApplicationContext(moduleForHost(), {
    logger: false,
    abortOnError: false
  });
  try {
    await assert.rejects(
      NestFactory.createApplicationContext(moduleForHost(), { logger: false, abortOnError: false }),
      framework.ZLinkConfigurationException
    );
  } finally {
    await owner.close();
  }
});

test('plain runtime start rejects RoutingId conflict', async () => {
  const store = new framework.ZLinkInMemoryProviderLocationStore();
  function runtime() {
    return new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration(hostOptions(store))
    });
  }
  const owner = runtime();
  const conflict = runtime();
  await owner.start();
  try {
    await assert.rejects(conflict.start(), framework.ZLinkConfigurationException);
  } finally {
    await owner.shutdown();
  }
});
