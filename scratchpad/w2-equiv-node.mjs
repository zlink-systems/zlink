// W-2 byte-equivalence proof: generated pilot codecs vs hand codecs (Node).
// Invokes the REAL hand codec functions from the built framework package
// (dist/, newer than src/) - not a copied/hardcoded expected-bytes string.
// Run with: node --experimental-strip-types w2-equiv-node.mjs
import { createRequire } from 'node:module';
const require = createRequire(import.meta.url);
const FRAMEWORK = '/home/hep7/project/zlink/framework/languages/node/packages/framework/dist/runtime/foundation';
const m6a = require(`${FRAMEWORK}/service-wire-m6a-codec.js`);
const stateful = require(`${FRAMEWORK}/service-stateful-wire-codec.js`);
const wireCodec = require(`${FRAMEWORK}/service-wire-codec.js`);
const constants = require(`${FRAMEWORK}/service-wire-constants.generated.js`);

const gen = await import(
  '/home/hep7/project/zlink/framework/runtime/protocol/generated/node/service_wire_pilot_codec.generated.ts'
);

function hex(bytes) { return Buffer.from(bytes).toString('hex'); }

const livenessCodec = wireCodec.createServiceWireCodec({
  magic: constants.SERVICE_WIRE_MAGIC,
  major: constants.SERVICE_WIRE_MAJOR,
  commands: { livenessProbe: constants.ServiceWireCommand.livenessProbe, livenessAck: constants.ServiceWireCommand.livenessAck },
});

const results = [];
const check = (name, handBytes, genBytes) => {
  const h = hex(handBytes);
  const g = hex(genBytes);
  results.push({ name, hand: h, gen: g, equal: h === g });
};

check('livenessProbe(5)',
  livenessCodec.encodeLivenessRecord({ command: constants.ServiceWireCommand.livenessProbe, probeId: 42n }),
  gen.encodeLivenessProbe5({ probeId: 42n }));

check('livenessAck(6)',
  livenessCodec.encodeLivenessRecord({ command: constants.ServiceWireCommand.livenessAck, probeId: 42n }),
  gen.encodeLivenessAck6({ probeId: 42n }));

check('nodeSend(16)', m6a.encodeNodeSendHeader(), gen.encodeNodeSend16());

check('nodeRequest(17)', m6a.encodeNodeRequestHeader(7n), gen.encodeNodeRequest17({ correlation: 7n }));

check('channelSend(18)', m6a.encodeChannelSendHeader('lobby'), gen.encodeChannelSend18({ channelName: 'lobby' }));

check('channelRequest(19)',
  m6a.encodeChannelRequestHeader(7n, 'lobby'),
  gen.encodeChannelRequest19({ correlation: 7n, channelName: 'lobby' }));

check('logicalMulticast(23)',
  stateful.encodeLogicalMulticastHeader('lobby', 'topicA', 'spot1'),
  gen.encodeLogicalMulticast23({ channelName: 'lobby', topic: 'topicA', sourceSpotId: 'spot1' }));

check('actorLookup(26)',
  stateful.encodeActorLookupHeader(7n, 'actor1'),
  gen.encodeActorLookup26({ correlation: 7n, actorId: 'actor1' }));

const actorFence = {
  actor: { nodeRid: 'nrid', actorId: 'actor1', generation: 2n },
  targetNodeGeneration: 3n,
  authorityOwnerGeneration: 4n,
  ownerLeaseGeneration: 5n,
};
const genFence = {
  id: 'actor1', generation: 2n,
  targetNodeRid: Buffer.from('nrid', 'utf8'),
  targetNodeGeneration: 3n, expectedAuthorityOwnerGeneration: 4n, expectedOwnerLeaseGeneration: 5n,
};
check('actorDestroy(27)',
  stateful.encodeActorDestroyHeader(7n, actorFence),
  gen.encodeActorDestroy27({ correlation: 7n, actor: genFence }));

let allEqual = true;
for (const r of results) {
  console.log(`${r.name}: ${r.equal ? 'IDENTICAL' : 'DIFFERS'}`);
  console.log(`  hand: ${r.hand}`);
  console.log(`  gen : ${r.gen}`);
  if (!r.equal) allEqual = false;
}

// Decode round-trip: feed each hand-encoded frame into the matching
// generated decoder and confirm it reproduces the same input fields.
console.log('\n-- decode round-trip (generated decoder fed the hand-encoded bytes) --');
const decodeChecks = [
  ['livenessProbe(5)', () => gen.decodeLivenessProbe5(livenessCodec.encodeLivenessRecord({ command: constants.ServiceWireCommand.livenessProbe, probeId: 42n })), { probeId: 42n }],
  ['nodeRequest(17)', () => gen.decodeNodeRequest17(m6a.encodeNodeRequestHeader(7n)), { correlation: 7n }],
  ['channelSend(18)', () => gen.decodeChannelSend18(m6a.encodeChannelSendHeader('lobby')), { channelName: 'lobby' }],
  ['logicalMulticast(23)', () => gen.decodeLogicalMulticast23(stateful.encodeLogicalMulticastHeader('lobby', 'topicA', 'spot1')), { channelName: 'lobby', topic: 'topicA', sourceSpotId: 'spot1' }],
  ['actorLookup(26)', () => gen.decodeActorLookup26(stateful.encodeActorLookupHeader(7n, 'actor1')), { correlation: 7n, actorId: 'actor1' }],
];
for (const [name, run, expected] of decodeChecks) {
  const decoded = run();
  const ok = JSON.stringify(decoded, (_, v) => (typeof v === 'bigint' ? v.toString() : v))
    === JSON.stringify(expected, (_, v) => (typeof v === 'bigint' ? v.toString() : v));
  console.log(`${name} decode: ${ok ? 'ROUND-TRIPS' : 'MISMATCH'}`);
  if (!ok) allEqual = false;
}

process.exit(allEqual ? 0 : 1);
