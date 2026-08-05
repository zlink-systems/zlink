// SM-D5: Physical disconnect를 current bindings 전체에 통지한다 시나리오를 검증한다.
import {
  bindActor,
  createSessionClient,
  pingActor,
  waitEvidence
} from '../Support/session-binding-support';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

export async function runSmD5(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const localActorId = `actor-sm-d5-local-${suffix}`;
  const failingActorId = `actor-sm-d5-fail-${suffix}`;
  const remoteActorId = `actor-sm-d5-remote-${suffix}`;
  const client = createSessionClient(options.sessionAStreamEndpoint);
  await client.connect();
  const local = await bindActor(client, localActorId, 'session-a');
  const failing = await bindActor(client, failingActorId, 'play-a');
  const remote = await bindActor(client, remoteActorId, 'play-b');
  try {
    ensure(
      local.generation !== undefined
      && failing.generation !== undefined
      && remote.generation !== undefined,
      'SM-D5 bind did not return exact Actor generations.'
    );
  } finally {
    await client.close();
  }

  const localEvidence = await waitEvidence(
    actorOwnerUrl(options, local.nodeRid),
    `entry-disconnected|rid=${local.nodeRid}|actor=${localActorId}`
  );
  const sessionEvidence = await waitEvidence(
    options.sessionAUrl,
    'session-disconnected|rid=session-a|'
  );
  const failedEvidence = await waitEvidence(
    actorOwnerUrl(options, failing.nodeRid),
    `entry-disconnected|rid=${failing.nodeRid}|actor=${failingActorId}`
  );
  const remoteEvidence = await waitEvidence(
    actorOwnerUrl(options, remote.nodeRid),
    `entry-disconnected|rid=${remote.nodeRid}|actor=${remoteActorId}`
  );
  ensure(
    localEvidence.length > 0
      && sessionEvidence.length > 0
      && failedEvidence.length > 0
      && remoteEvidence.length > 0,
    'SM-D5 automatic all-settled fan-out did not reach every captured Actor.'
  );

  const rebound = createSessionClient(options.sessionBStreamEndpoint);
  await rebound.connect();
  try {
    const current = await bindActor(rebound, remoteActorId, 'play-b');
    ensure(
      current.generation === remote.generation,
      'SM-D5 physical disconnect changed Actor ObjectGeneration.'
    );
    const reply = await pingActor(rebound, remoteActorId, 'after-physical-cleanup');
    ensure(reply.actorId === remoteActorId, 'SM-D5 callback failure prevented binding cleanup or rebind.');
  } finally {
    await rebound.close();
  }

  console.log('scenario SM-D5 passed');
}

function actorOwnerUrl(options: ClientOptions, nodeRid: string): string {
  if (nodeRid === 'session-a') return options.sessionAUrl;
  if (nodeRid === 'session-b') return options.sessionBUrl;
  if (nodeRid === 'play-a') return options.playAUrl;
  if (nodeRid === 'play-b') return options.playBUrl;
  throw new Error(`Unexpected Actor owner '${nodeRid}'.`);
}
