'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '..', '..');

test('TicTacToe observes peer readiness only through the public RouteMesh runtime', () => {
  const playMain = fs.readFileSync(path.join(
    nodeRoot,
    'samples/TicTacToe.Ts/Server/Play/main.ts'
  ), 'utf8');

  assert.match(playMain, /ZLINK_ROUTE_MESH_RUNTIME/);
  assert.match(playMain, /ZLinkRouteMeshRuntime/);
  assert.match(playMain, /peer\.state === ZLinkPeerState\.Ready/);
  assert.doesNotMatch(playMain, /\bspotNodeRuntime\b/);
  assert.doesNotMatch(playMain, /\bprimaryMeshNode\b/);
  assert.doesNotMatch(playMain, /admittedPeerCount/);
  assert.doesNotMatch(playMain, /as unknown as/);
});
