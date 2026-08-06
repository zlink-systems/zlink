import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from './client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from './scenario-assert';

/** Runs a scenario through the consumer public request surface and provider evidence. */
export async function runDiscoveryCoverage(options: ClientOptions, scenario: string): Promise<void> {
  const value = scenario.toLowerCase() + '-' + Date.now().toString(36);
  const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value, marker: scenario });
  ensure(reply.value === 'profile:' + value, scenario + ' profile reply mismatch.');

  const providerUrls = [options.providerAUrl, ...(options.providerBUrl === undefined ? [] : [options.providerBUrl])];
  const evidence = (await Promise.all(providerUrls.map((url) => getJson<readonly string[]>(url, '/evidence')))).flat();
  ensure(evidence.some((line) => line.includes('value=' + value)), scenario + ' provider evidence is missing.');
  console.log('scenario ' + scenario + ' passed');
}
