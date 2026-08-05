// SM-C1: Channel handler에서 Spot request를 보낸다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotSlowRouteRes,
  SpotSlowRouteReq,
  SpotStateMsgRes,
  SpotStateMsgReq,
  SpotStateRouteReq,
  StateRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmC1(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-c1-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    created.spotId === spotId && created.nodeRid === 'play-a',
    'SM-C1 spot was not created on play-a.'
  );

  const failures: string[] = [];
  let viaChannel: StateRes | undefined;
  try {
    viaChannel = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
      spotId,
      operation: 'noop',
      delta: 0
    } satisfies SpotStateRouteReq);
    ensure(viaChannel.spotId === spotId, 'SM-C1 request reached the wrong spot.');
    ensure(viaChannel.nodeRid === 'play-a', 'SM-C1 request reached the wrong node.');
  } catch (error) {
    failures.push(`request=${formatError(error)}`);
  }

  try {
    const command = await postJson<SpotStateMsgRes>(options.playAUrl, '/spot/state/command', {
      spotId,
      marker: 'sm-c1-send'
    } satisfies SpotStateMsgReq);
    ensure(command.accepted, 'SM-C1 external channel command was not accepted.');
  } catch (error) {
    failures.push(`send=${formatError(error)}`);
  }

  try {
    const timeout = await postJson<SpotSlowRouteRes>(options.playAUrl, '/spot/slow/request', {
      spotId,
      marker: 'sm-c1-timeout',
      delayMs: 1500,
      timeoutMs: 100
    } satisfies SpotSlowRouteReq);
    ensure(timeout.timedOut, 'SM-C1 expected slow channel-to-spot request to time out.');
  } catch (error) {
    failures.push(`timeout=${formatError(error)}`);
  }

  try {
    const afterTimeout = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
      spotId,
      operation: 'add',
      delta: 1
    } satisfies SpotStateRouteReq);
    ensure(afterTimeout.spotId === spotId, 'SM-C1 post-timeout request reached the wrong spot.');
    ensure(afterTimeout.nodeRid === 'play-a', 'SM-C1 post-timeout request reached the wrong node.');
    if (viaChannel !== undefined) {
      ensure(afterTimeout.value > viaChannel.value, 'SM-C1 post-timeout state did not advance.');
    }
  } catch (error) {
    failures.push(`postTimeout=${formatError(error)}`);
  }

  if (failures.length === 0) {
    const expectedEvidence = [`spot-state-command|rid=play-a|spot=${spotId}|marker=sm-c1-send`];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-C1 evidence mismatch.'
    );
    console.log('scenario SM-C1 passed');
    return;
  }

  throw new Error(`SM-C1 channel-to-spot messaging incomplete: ${failures.join('; ')}`);
}

function formatError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
