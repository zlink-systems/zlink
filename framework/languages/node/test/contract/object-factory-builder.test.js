const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const publicFramework = require('../../packages/framework/dist');
const nestjs = require('../../packages/nestjs/dist');

class RoomSpot {}
class MatchSpot {}
class PlayerActorFactory {}
class RoomRelocationAdapter {}

function configureFramework(configure) {
  return framework.createFrameworkOptions((options) => {
    const server = options.addRouteMesh('game').objects().server();
    configure(server);
  });
}

test('factory configure callback stores one selected relocation policy', () => {
  const options = configureFramework((server) => {
    server.addSpotFactory(
      'room',
      RoomSpot,
      (factory) => factory.preserveStateWith(RoomRelocationAdapter)
    );
    server.addInstanceSpotFactory(
      'match',
      MatchSpot,
      (factory) => factory.disableRelocation()
    );
    server.addActorFactory(
      'player',
      PlayerActorFactory,
      (factory) => factory.recreateOnRelocation()
    );
  });

  const node = options.spotNodes.game;
  assert.equal(node.spotFactoryRegistrations.room.relocation.kind, 'snapshot');
  assert.equal(
    node.spotFactoryRegistrations.room.relocation.adapterType,
    RoomRelocationAdapter
  );
  assert.equal(
    node.instanceSpotFactoryRegistrations.match.relocation.kind,
    'disabled'
  );
  assert.equal(node.actorFactoryRegistrations.player.relocation.kind, 'recreate');
});

test('factory configure callback rejects missing or repeated policy selection', () => {
  assert.throws(
    () => configureFramework((server) => {
      server.addSpotFactory('missing', RoomSpot, () => {});
    }),
    /exactly one relocation policy/
  );

  assert.throws(
    () => configureFramework((server) => {
      server.addActorFactory('duplicate', PlayerActorFactory, (factory) => {
        factory.disableRelocation();
        factory.recreateOnRelocation();
      });
    }),
    /exactly one relocation policy/
  );
});

test('factory builder is sealed after the configure callback returns', () => {
  let escapedBuilder;
  configureFramework((server) => {
    server.addSpotFactory('sealed', RoomSpot, (factory) => {
      escapedBuilder = factory;
      factory.disableRelocation();
    });
  });

  assert.throws(
    () => escapedBuilder.stableTypeLimit(10),
    /cannot be changed after the configure callback returns/
  );
  assert.throws(
    () => escapedBuilder.recreateOnRelocation(),
    /cannot be changed after the configure callback returns/
  );
});

test('explicit stable type limits must be positive signed 32-bit integers', () => {
  for (const limit of [0, -1, 2_147_483_648]) {
    assert.throws(
      () => configureFramework((server) => {
        server.addSpotFactory('limited', RoomSpot, (factory) => {
          factory.stableTypeLimit(limit);
          factory.disableRelocation();
        });
      }),
      /integer from 1 through 2147483647/
    );
  }
});

test('NestJS factory builder applies the same callback contract', () => {
  const options = nestjs.zlinkFramework();
  const server = options.addRouteMesh('game').objects().server();
  let escapedBuilder;
  server.addSpotFactory(
    'room',
    RoomSpot,
    (factory) => {
      escapedBuilder = factory;
      factory.disableRelocation();
    }
  );
  assert.throws(
    () => escapedBuilder.recreateOnRelocation(),
    /cannot be changed after the configure callback returns/
  );
  assert.throws(
    () => server.addActorFactory('missing', PlayerActorFactory, () => {}),
    /exactly one relocation policy/
  );
});

test('legacy relocation policy factories are not public exports', () => {
  for (const name of [
    'zlinkDisabledRelocation',
    'zlinkRecreateRelocation',
    'zlinkSnapshotRelocation'
  ]) {
    assert.equal(name in publicFramework, false);
  }
});

test('object server builder is the only object registration surface', () => {
  let frameworkMesh;
  framework.createFrameworkOptions((options) => {
    frameworkMesh = options.addRouteMesh('game');
  });
  const options = nestjs.zlinkFramework();
  const nestMesh = options.addRouteMesh('game');
  for (const name of [
    'addEntrySpot',
    'addSpotFactory',
    'addInstanceSpotFactory',
    'actorFactory'
  ]) {
    assert.equal(name in frameworkMesh, false);
    assert.equal(name in nestMesh, false);
  }
});
