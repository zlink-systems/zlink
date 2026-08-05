// SM-A8: CPU worker 결과를 Spot state에 반영한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  SpotStateRouteReq,
  SpotWorkerCompleteRes,
  SpotWorkerCompleteReq,
  SpotWorkerStartReq,
  StateRes,
  WorkerStartRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA8(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a8-${Date.now().toString(36)}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-A8 worker spot was not created.');
  ensure(created.nodeRid === 'play-a', 'SM-A8 worker spot was created on the wrong node.');

  const worker = await postJson<WorkerStartRes>(options.playAUrl, '/spot/worker/start', {
    spotId,
    marker: 'sm-a8-worker',
    delayMs: 500
  } satisfies SpotWorkerStartReq);
  ensure(worker.spotId === spotId, 'SM-A8 worker start target mismatch.');

  const duringWorker = await postJson<StateRes>(options.playAUrl, '/spot/state/local', {
    spotId,
    operation: 'add',
    delta: 1
  } satisfies SpotStateRouteReq);
  ensure(duringWorker.value === 1, 'SM-A8 concurrent spot turn did not run before worker completion.');

  const completed = await postJson<SpotWorkerCompleteRes>(options.playAUrl, '/spot/worker/complete', {
    spotId,
    marker: 'sm-a8-worker'
  } satisfies SpotWorkerCompleteReq);
  ensure(completed.completed, 'SM-A8 worker did not complete.');

  const workerStartIndex = findIndex(completed.evidence, `worker-start|rid=play-a|spot=${spotId}|marker=sm-a8-worker`);
  const stateIndex = findIndex(completed.evidence, `spot-state-request|rid=play-a|spot=${spotId}|value=1`);
  const workerCompleteIndex = findIndex(completed.evidence, `worker-complete|rid=play-a|spot=${spotId}|marker=sm-a8-worker`);
  ensure(
    workerStartIndex >= 0 && stateIndex > workerStartIndex && workerCompleteIndex > stateIndex,
    'SM-A8 expected spot turn evidence between worker start and completion.'
  );

  console.log('scenario SM-A8 passed');
}

function findIndex(lines: readonly string[], marker: string): number {
  return lines.findIndex((line) => line.includes(marker));
}
