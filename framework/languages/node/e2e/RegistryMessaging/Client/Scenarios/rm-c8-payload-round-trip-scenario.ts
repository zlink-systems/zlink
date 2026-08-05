// RM-C8: RouteMesh SS payload 무결성 시나리오를 검증한다.
import type { PayloadRes, ProfileRes } from '../../Shared/messages';
import { sha256Hex } from '../../Shared/messages';
import { getJson, postJson } from '../../../http-client';
import { countNewEvidence, ensure, uniqueMarker } from '../Support/scenario-assert';

export async function runRmC8(
  singleConsumerUrl: string,
  directConsumerUrl: string,
  providerAUrl: string
): Promise<void> {
  const before = await getJson<string[]>(providerAUrl, '/evidence');
  const markers: string[] = [];
  for (const size of [1, 4096, 256 * 1024, 1024 * 1024]) {
    const marker = uniqueMarker(`rm-c8-${size}`);
    markers.push(marker);
    const payload = buildPayload(size);
    const reply = await postJson<PayloadRes>(singleConsumerUrl, '/profile/payload', { marker, payload });
    ensure(reply.marker === marker, 'RM-C8 marker mismatch.');
    ensure(reply.length === payload.length, 'RM-C8 payload length mismatch.');
    ensure(reply.sha256 === sha256Hex(payload), 'RM-C8 payload hash mismatch.');
  }

  const followUp = await postJson<ProfileRes>(directConsumerUrl, '/profile/request', { value: 'rm-c8-after' });
  ensure(followUp.value === 'profile:rm-c8-after', 'RM-C8 follow-up request failed.');
  const after = await getJson<string[]>(providerAUrl, '/evidence');
  ensure(markers.every((marker) => countNewEvidence(after, before, 'payload-request|rid=api-a', marker) === 1), 'RM-C8 payload evidence missing.');
  console.log('scenario RM-C8 passed');
}

function buildPayload(size: number): string {
  let payload = '';
  for (let i = 0; i < size; i += 1) {
    payload += String.fromCharCode('a'.charCodeAt(0) + (i % 26));
  }
  return payload;
}
