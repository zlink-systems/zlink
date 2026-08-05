// SM-C3: Spot에서 다른 Spot으로 request를 보낸다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotToSpotNegativeRes,
  SpotToSpotNegativeRouteReq,
  SpotToSpotRes,
  SpotToSpotRouteReq,
  SpotToSpotTimeoutRes,
  SpotToSpotTimeoutRouteReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmC3(options: ClientOptions): Promise<void> {
  const sourceSpotId = `spot-sm-c3-source-${Date.now()}`;
  const targetSpotId = `spot-sm-c3-target-${Date.now()}`;
  for (const spotId of [sourceSpotId, targetSpotId]) {
    const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
      spotId
    } satisfies CreateSpotReq);
    ensure(
      created.spotId === spotId && created.nodeRid === 'play-a',
      'SM-C3 spot was not created on play-a.'
    );
  }

  const failures: string[] = [];
  try {
    const direct = await postJson<SpotToSpotRes>(options.playAUrl, '/spot/to-spot/request', {
      sourceSpotId,
      targetSpotId,
      marker: 'direct'
    } satisfies SpotToSpotRouteReq);
    ensure(direct.sourceSpotId === sourceSpotId, 'SM-C3 source spot mismatch.');
    ensure(direct.targetSpotId === targetSpotId, 'SM-C3 target spot mismatch.');
    ensure(direct.targetValue >= 3, 'SM-C3 target state was not updated.');
  } catch (error) {
    failures.push(`request=${formatError(error)}`);
  }

  try {
    const timeout = await postJson<SpotToSpotTimeoutRes>(options.playAUrl, '/spot/to-spot/timeout', {
      sourceSpotId,
      targetSpotId,
      marker: 'slow'
    } satisfies SpotToSpotTimeoutRouteReq);
    ensure(timeout.failed, 'SM-C3 slow target request did not time out.');
  } catch (error) {
    failures.push(`timeout=${formatError(error)}`);
  }

  try {
    const negative = await postJson<SpotToSpotNegativeRes>(options.playAUrl, '/spot/to-spot/negative', {
      sourceSpotId,
      targetSpotId,
      marker: 'missing'
    } satisfies SpotToSpotNegativeRouteReq);
    ensure(negative.requestFailed, 'SM-C3 missing target handler request did not fail.');
  } catch (error) {
    failures.push(`negative=${formatError(error)}`);
  }

  if (failures.length === 0) {
    const expectedEvidence = [
      `spot-to-spot|rid=play-a|source=${sourceSpotId}|target=${targetSpotId}|value=`,
      `spot-state-command|rid=play-a|spot=${targetSpotId}|marker=sm-c3-send-direct`,
      `spot-msg|rid=play-a|spot=${targetSpotId}|marker=sm-c3-publish-direct`,
      `spot-to-spot-timeout|rid=play-a|source=${sourceSpotId}|target=${targetSpotId}|failed=True`,
      `spot-to-spot-negative|rid=play-a|source=${sourceSpotId}|target=${targetSpotId}|requestFailed=True`,
      'dispatch-error|surface=spot|kind=request|reason=no_handler|action=reply_error|packet=MissingSpotReq',
      'dispatch-error|surface=spot|kind=send|reason=no_handler|action=drop|packet=MissingSpotMsg'
    ];
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 30000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-C3 evidence mismatch.'
    );
    console.log('scenario SM-C3 passed');
    return;
  }

  throw new Error(`SM-C3 spot-to-spot messaging incomplete: ${failures.join('; ')}`);
}

function formatError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
