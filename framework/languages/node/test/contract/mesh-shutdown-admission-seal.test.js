const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

for (const withChannel of [true, false]) {
  test(`host shutdown seals raw mesh Hello and preserves Draining Update (channel=${withChannel})`, async t => {
    const meshName = `shutdown-seal-${withChannel}-${process.pid}`;
    const registration = framework.createFrameworkRegistrationWithBuilder(builder => {
      const mesh = builder.addRouteMesh(meshName)
        .listen(`inproc://${meshName}`).routingId('sealed-node');
      if (withChannel) mesh.channel('work').server();
    });
    const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
    let claim;
    let shutdown;
    try {
      await host.start();
      const backend = host.spotNodeRuntime.meshNode(meshName);
      const raw = backend.runtime;
      const incoming = [];
      const sent = [];
      let resolvePublished;
      const published = new Promise(resolve => { resolvePublished = resolve; });
      t.mock.method(raw.router, 'receive', () => incoming.shift());
      t.mock.method(raw.router, 'send', async (target, parts) => {
        sent.push({ target, parts: parts.map(part => Buffer.from(part)) });
        if (wire.decodeHeader(parts[0]).command === wire.M6aServiceWireCommand.update) resolvePublished();
      });
      const local = raw.topology.localDescriptor();
      const admitted = {
        ...local, nodeRoutingId: 'admitted-peer', advertisedEndpoint: 'inproc://admitted-peer'
      };
      const newcomer = {
        ...local, nodeRoutingId: 'new-peer', advertisedEndpoint: 'inproc://new-peer'
      };
      const receive = async (peer, parts) => {
        incoming.push({ sourceRid: peer.nodeRoutingId, parts, close() {} });
        return raw.pumpOne();
      };
      assert.equal(await receive(admitted, [wire.encodeRouteMeshAdmission(
        wire.M6aServiceWireCommand.hello, admitted
      )]), 'infrastructure');
      assert.equal(wire.decodeHeader(sent.at(-1).parts[0]).command, wire.M6aServiceWireCommand.admit);
      assert(raw.topology.peer(admitted.nodeRoutingId));
      raw.expectPeerByRoutingId(newcomer.advertisedEndpoint, newcomer.nodeRoutingId);
      assert.equal(await raw.announceExpectedPeers(), 1);
      sent.length = 0;

      claim = host.admission.claim(meshName, 'accepted work held during shutdown');
      shutdown = host.shutdown({ deadlineMs: 1000 });
      await published;
      assert.equal(host.admission.accepts(meshName), false);
      assert.equal(await raw.announceExpectedPeers(), 0);
      assert.equal(await raw.announcePeer(newcomer.nodeRoutingId), false);
      for (const parts of [[], [Buffer.alloc(0)], [wire.encodeRouteMeshAdmission(
        wire.M6aServiceWireCommand.hello, newcomer
      )]]) {
        assert.equal(await receive(newcomer, parts), 'dropped');
      }
      assert.equal(raw.topology.peer(newcomer.nodeRoutingId), undefined);
      assert.equal(await receive(admitted, [wire.encodeRouteMeshAdmission(
        wire.M6aServiceWireCommand.update,
        { ...admitted, descriptorRevision: admitted.descriptorRevision + 1n, state: 'draining' }
      )]), 'infrastructure');
      assert.equal(raw.topology.peer(admitted.nodeRoutingId).descriptor.state, 'draining');
      assert.equal(sent.length, 1, 'shutdown publishes one descriptor even without channels');
      assert.equal(sent[0].target, admitted.nodeRoutingId);
      const update = wire.decodeRouteMeshAdmission(
        sent[0].parts[0], wire.M6aServiceWireCommand.update, local.nodeRoutingId
      );
      assert.equal(update.state, 'draining');
      assert(update.channels.every(channel => channel.weight === 0));
      assert.equal(raw.topology.localDescriptor().state, 'draining');
      claim.close();
      claim = undefined;
      assert.equal((await shutdown).outcome, framework.ZLinkFrameworkTerminationOutcome.Stopped);
    } finally {
      claim?.close();
      if (shutdown !== undefined) await shutdown;
      await host.stop();
    }
  });
}
