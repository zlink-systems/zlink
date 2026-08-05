// RM-C1: Request와 send의 완료 의미를 구분한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';

export async function runRmC1(providerAUrl: string, providerBUrl: string): Promise<void> {
  const reply = await postJson<ProfileRes>(providerAUrl, '/profile/request', { value: 'rm-c1-request' });
  ensure(reply.value === 'profile:rm-c1-request', 'RM-C1 request reply mismatch.');

  const commandId = uniqueMarker('cmd');
  await postJson(providerAUrl, '/profile/command', { commandId });
  const lines = await waitForAnyCommandEvidence([providerAUrl, providerBUrl], commandId);
  ensure(lines.some((line) => line.includes('profile-command|')), 'RM-C1 send evidence missing.');
  console.log('scenario RM-C1 passed');
}

async function waitForAnyCommandEvidence(providerUrls: readonly string[], commandId: string): Promise<string[]> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    const snapshots = await Promise.all(providerUrls.map((url) => getEvidenceSnapshot(url)));
    const lines = snapshots.flat();
    if (lines.some((line) => line.includes(commandId))) {
      return lines;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  return (await Promise.all(providerUrls.map((url) => getEvidenceSnapshot(url)))).flat();
}

async function getEvidenceSnapshot(providerUrl: string): Promise<string[]> {
  try {
    return await Promise.race([
      getJson<string[]>(providerUrl, '/evidence'),
      new Promise<string[]>((resolve) => setTimeout(() => resolve([]), 500))
    ]);
  } catch {
    return [];
  }
}
