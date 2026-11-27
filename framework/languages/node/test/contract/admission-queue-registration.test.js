'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const {
  createFrameworkRegistration
} = require('../../packages/framework/dist/contracts/Configuration/Registration');
const { ZLinkFrameworkRuntimeHost } = require('../../packages/framework/dist/runtime/host');
const { spotType } = require('../../../../runtime/conformance/admission-queue-messages-v1.json');

function createHost(objectRole, locations = { useInMemoryStores: true }) {
  const registration = createFrameworkRegistration({
    locations,
    spotNodes: { game: { objectRole, router: { bind: 'tcp://127.0.0.1:0' } } },
    streamNodes: {
      login: {
        bind: 'tcp://127.0.0.1:0',
        session: class Session {},
        authenticationPacketName: 'Login',
        admissionQueueEnabled: true,
        admissionQueueKey: Buffer.alloc(32, 1).toString('base64url')
      }
    }
  });
  return new ZLinkFrameworkRuntimeHost({ registration });
}

test('only the Object Server registers the admission queue Instance Spot type', () => {
  const client = createHost('client').options.registration;
  const server = createHost('server').options.registration;
  assert.equal(client.admissionQueueRoute.meshName, 'game');
  assert.equal(
    client.spotNodes.get('game').instanceSpotFactoryRegistrations?.[spotType],
    undefined
  );
  assert.deepEqual(
    Object.keys(client.spotNodes.get('game').instanceSpotFactoryRegistrations ?? {}),
    []
  );
  assert.equal(
    server.spotNodes.get('game').instanceSpotFactoryRegistrations[spotType].relocation.kind,
    'recreate'
  );
});

test('startup rejects an admission queue mesh without an Object role or Location Store', () => {
  assert.throws(() => createHost(undefined), {
    name: 'ZLinkConfigurationException',
    message: /Object role/
  });
  assert.throws(() => createHost('server', {}), {
    name: 'ZLinkConfigurationException',
    message: /Location Store/
  });
});
