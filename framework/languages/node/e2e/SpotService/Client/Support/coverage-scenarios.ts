import type { CreateSpotRes, CreateSpotReq, EvidenceWaitReq } from '../../Shared/messages';
import type { ClientOptions } from './client-options';
import { postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

/** Runs an ungrouped scenario through the role public Entry Spot operation and evidence. */
export async function runSpotServiceCoverage(options: ClientOptions, scenario: string): Promise<void> {
  const spotId = scenario.toLowerCase() + '-' + Date.now().toString(36);
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', { spotId } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, scenario + ' did not create the requested Spot.');
  const evidence = await postJson<readonly string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: ['create-spot|rid=play-a|spot=' + spotId],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(evidence.some((line) => line.includes('create-spot|rid=play-a|spot=' + spotId)), scenario + ' Spot evidence is missing.');
  console.log('scenario ' + scenario + ' passed');
}

