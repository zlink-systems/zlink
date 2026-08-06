import type { ClientOptions } from './client-options';

/** Prevents a not-yet-ported Config 6 scenario from becoming a smoke test. */
export async function runDiscoveryCoverage(_options: ClientOptions, scenario: string): Promise<void> {
  throw new Error(`${scenario} is not implemented by the Config 6 Node fixture.`);
}
