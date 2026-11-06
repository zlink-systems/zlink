const assert = require('node:assert/strict');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const { fixedRidHostClock, finishFixedRidHostStartup } = require('../helpers/fixed-rid-host-clock');

function hostOptions(store) {
  const options = nestjs.zlinkFramework().addLocationStore(store);
  options.configureLocations().ownerLeaseFencingMarginMs(1);
  options
    .addRouteMesh('startup-conflict')
    .listen('tcp://127.0.0.1:*')
    .routingId('startup-conflict');
  return options.build();
}

test('Nest context rejects runtime startup RoutingId conflict', async (t) => {
  const hostClock = fixedRidHostClock(t);
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
    await finishFixedRidHostStartup(
      owner.get(nestjs.ZLINK_FRAMEWORK_RUNTIME),
      hostClock,
      framework.ZLinkFrameworkRuntimeState.Preparing
    );
    await assert.rejects(
      NestFactory.createApplicationContext(moduleForHost(), { logger: false, abortOnError: false }),
      framework.ZLinkConfigurationException
    );
  } finally {
    await owner.close();
  }
});

test('plain runtime start rejects RoutingId conflict', async (t) => {
  const hostClock = fixedRidHostClock(t);
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
    await finishFixedRidHostStartup(
      owner,
      hostClock,
      framework.ZLinkFrameworkRuntimeState.Preparing
    );
    await assert.rejects(conflict.start(), framework.ZLinkConfigurationException);
  } finally {
    await owner.shutdown();
  }
});
