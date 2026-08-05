#!/usr/bin/env node
'use strict';

const {
  ZLinkLocationAutoConnectType,
  ZLinkLocationWriteIntent,
  ZLinkLocationRole
} = require('@zlink-systems/framework');
const { ZLinkRedisLocationStore } = require('@zlink-systems/framework-locations-redis');
const fs = require('node:fs');

async function main() {
  const [action, redisEndpoint, keyPrefix, nodeRid, snapshotFile] = process.argv.slice(2);
  if (!['remove', 'restore'].includes(action)
    || redisEndpoint === undefined || keyPrefix === undefined || nodeRid === undefined || snapshotFile === undefined) {
    throw new Error('Usage: peer-fault.js <remove|restore> <redis-endpoint> <key-prefix> <node-rid> <snapshot-file>');
  }

  const store = new ZLinkRedisLocationStore({
    url: `redis://${redisEndpoint}`,
    keyPrefix
  });
  try {
    if (action === 'restore') {
      const peers = JSON.parse(fs.readFileSync(snapshotFile, 'utf8'), (key, value) => {
        if (value?.type === 'bigint') return BigInt(value.value);
        if (key === 'updatedAt') return new Date(value);
        return value;
      });
      for (const peer of peers) {
        const result = await store.updatePeer(peer, ZLinkLocationWriteIntent.NewClaim);
        if (result.status !== 'stored') {
          throw new Error(`Peer row restore failed with '${result.status}'.`);
        }
      }
      console.log(`restored-peer-rows=${peers.length}`);
      return;
    }

    const peers = await store.listPeers({
      autoConnectType: ZLinkLocationAutoConnectType.RouteMesh,
      meshName: 'to-actor',
      role: ZLinkLocationRole.Spot,
      nodeRid
    });
    if (peers.length === 0) {
      throw new Error(`No to-actor RouteMesh peer row exists for '${nodeRid}'.`);
    }
    const snapshot = peers.map((peer) => ({
      ...peer,
      nodeRid: String(peer.nodeRid),
      updatedAt: peer.updatedAt.toISOString()
    }));
    fs.writeFileSync(snapshotFile, JSON.stringify(snapshot, (_key, value) => {
      if (typeof value === 'bigint') return { type: 'bigint', value: value.toString() };
      return value;
    }));
    for (const peer of peers) {
      await store.removePeer({
        autoConnectType: peer.autoConnectType,
        meshName: peer.meshName,
        role: peer.role,
        nodeRid: peer.nodeRid,
        endpoint: peer.endpoint
      }, {
        ownerId: peer.ownerId,
        generation: peer.generation
      });
    }
    console.log(`removed-peer-rows=${peers.length}`);
  } finally {
    await store.dispose();
  }
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
