// RM-A3: Object Client pair의 connection 필요 여부 시나리오를 검증한다.
import assert from 'node:assert/strict';
import { getJson, postJson } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';

interface PeerStatus {
  readonly rid: string;
  readonly state: string;
  readonly ready: boolean;
}

interface MeshStatus {
  readonly rid: string;
  readonly readyPeerCount: number;
  readonly channels: readonly {
    readonly channelName: string;
    readonly isReady: boolean;
    readonly readyTargetCount: number;
    readonly localWeight: number;
  }[];
  readonly peers: readonly PeerStatus[];
}

interface DirectOutcome {
  readonly terminal: string;
  readonly errorKind: string;
}

export async function runRmA3(options: ClientOptions): Promise<void> {
  const clientAUrl = requireOption(options.rmA3ClientAUrl, 'rmA3ClientAUrl');
  const clientBUrl = requireOption(options.rmA3ClientBUrl, 'rmA3ClientBUrl');
  const expectedState = requireOption(options.rmA3ExpectedState, 'rmA3ExpectedState');
  const expectedReady = options.rmA3ExpectedReady ?? false;
  const clientA = await waitForPeer(clientAUrl, 'client-b', expectedState, expectedReady);
  const clientB = await waitForPeer(clientBUrl, 'client-a', expectedState, expectedReady);
  assertPeer(clientA, 'client-b', expectedState, expectedReady);
  assertPeer(clientB, 'client-a', expectedState, expectedReady);
  assert.equal(clientA.readyPeerCount, expectedReady ? 1 : 0);
  assert.equal(clientB.readyPeerCount, expectedReady ? 1 : 0);
  if (options.rmA3ExpectedServerWeight !== undefined) {
    assert.ok(clientB.channels.some((channel) =>
      channel.channelName === 'registry.messaging.rm-a3'
      && channel.localWeight === options.rmA3ExpectedServerWeight));
  }

  if (options.rmA3CheckNodeDirect === true) {
    const outcome = await postJson<{
      readonly send: DirectOutcome;
      readonly request: DirectOutcome;
    }>(clientAUrl, '/rm-a3/node-direct', { targetRid: 'client-b' });
    for (const operation of [outcome.send, outcome.request]) {
      assert.equal(operation.terminal, 'NotFound');
      assert.equal(operation.errorKind, '0');
    }
  }

  const stableMilliseconds = options.rmA3StableMilliseconds ?? 0;
  const deadline = Date.now() + stableMilliseconds;
  let observations = 0;
  while (Date.now() < deadline) {
    const status = await getJson<MeshStatus>(clientAUrl, '/rm-a3/status');
    assertPeer(status, 'client-b', expectedState, expectedReady);
    observations += 1;
    await delay(100);
  }
  console.log(
    `scenario RM-A3 state=${expectedState} ready=${expectedReady}`
      + ` stableObservations=${observations}`
  );
}

async function waitForPeer(
  baseUrl: string,
  peerRid: string,
  expectedState: string,
  expectedReady: boolean
): Promise<MeshStatus> {
  let last: MeshStatus | undefined;
  for (let attempt = 0; attempt < 100; attempt += 1) {
    last = await getJson<MeshStatus>(baseUrl, '/rm-a3/status');
    const peer = last.peers.find((candidate) => candidate.rid === peerRid);
    if (peer?.state === expectedState && peer.ready === expectedReady) {
      return last;
    }
    await delay(100);
  }
  throw new Error(
    `Peer '${peerRid}' did not reach ${expectedState}; last=${JSON.stringify(last)}`
  );
}

function assertPeer(
  status: MeshStatus,
  peerRid: string,
  expectedState: string,
  expectedReady: boolean
): void {
  const matches = status.peers.filter((candidate) => candidate.rid === peerRid);
  assert.equal(matches.length, 1, `Peer '${peerRid}' must have exactly one public row.`);
  const peer = matches[0];
  assert.ok(peer, `Missing peer '${peerRid}'.`);
  assert.equal(peer.state, expectedState);
  assert.equal(peer.ready, expectedReady);
}

function requireOption(value: string | undefined, name: string): string {
  if (value === undefined) throw new Error(`${name} is required for RM-A3.`);
  return value;
}

function delay(milliseconds: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}
