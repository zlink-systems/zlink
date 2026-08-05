// SM-D5A: 선택한 Actor에 logical disconnect를 통지한다 시나리오를 검증한다.
import type { LogicalDisconnectReq, LogicalDisconnectRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';
import {
  bindActor,
  countEvidence,
  createSessionClient,
  expectStaleActorRoute,
  getEvidence,
  pingActor,
  waitEvidence
} from '../Support/session-binding-support';

export async function runSmD5A(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const selectedActorId = `actor-sm-d5a-selected-${suffix}`;
  const otherActorId = `actor-sm-d5a-other-${suffix}`;
  const client = createSessionClient(options.sessionAStreamEndpoint);
  await client.connect();
  try {
    await bindActor(client, selectedActorId, 'play-a');
    await bindActor(client, otherActorId, 'play-b');
    const disconnectEvidence =
      `session-disconnected|rid=session-a|`;
    const disconnectCountBefore = countEvidence(
      await getEvidence(options.sessionAUrl),
      disconnectEvidence
    );
    const result = await client
      .request({ actorId: selectedActorId } satisfies LogicalDisconnectReq)
      .packetName('LogicalDisconnectReq')
      .timeout(5000)
      .submit<LogicalDisconnectRes>();
    ensure(result.actorId === selectedActorId, 'SM-D5A logical disconnect Actor mismatch.');
    ensure(
      result.remainingActorIds.length === 1 && result.remainingActorIds[0] === otherActorId,
      'SM-D5A logical disconnect changed another Actor binding.'
    );

    await waitEvidence(
      options.playAUrl,
      `entry-disconnected|rid=play-a|actor=${selectedActorId}`
    );
    await expectStaleActorRoute(client, selectedActorId, 'selected-after-logical-disconnect');
    const other = await pingActor(client, otherActorId, 'other-after-logical-disconnect');
    ensure(other.actorId === otherActorId, 'SM-D5A physical connection or other binding was closed.');
    ensure(
      countEvidence(
        await getEvidence(options.sessionAUrl),
        disconnectEvidence
      ) === disconnectCountBefore,
      'SM-D5A logical notification closed the physical Session.'
    );
  } finally {
    await client.close();
  }

  console.log('scenario SM-D5A passed');
}
