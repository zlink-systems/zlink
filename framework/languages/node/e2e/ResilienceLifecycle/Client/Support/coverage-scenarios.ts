import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from './client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

/** Exercises the public request path while the lifecycle runner owns process faults. */
export async function runResilienceCoverage(options: ClientOptions, scenario: string): Promise<void> {
  const value = scenario.toLowerCase() + '-' + Date.now().toString(36);
  const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value, marker: scenario });
  ensure(reply.value === 'profile:' + value, scenario + ' profile reply mismatch.');
  const evidence = (await Promise.all([
    getJson<readonly string[]>(options.providerAUrl, '/evidence'),
    getJson<readonly string[]>(options.providerBUrl, '/evidence')
  ])).flat();
  ensure(evidence.some((line) => line.includes('value=' + value)), scenario + ' provider evidence is missing.');
  console.log('scenario ' + scenario + ' passed');
}

