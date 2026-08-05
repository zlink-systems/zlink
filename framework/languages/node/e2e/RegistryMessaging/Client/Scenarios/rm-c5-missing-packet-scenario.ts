// RM-C5: Handler가 없는 message를 처리한다 시나리오를 검증한다.
import type { ProfileRes, RequestFailureRes } from '../../Shared/messages';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runRmC5(locationConsumerUrl: string, providerAUrl: string, providerBUrl: string): Promise<void> {
  const missingRequest = await postJson<RequestFailureRes>(locationConsumerUrl, '/profile/missing-request', { value: 'missing-request' });
  ensure(
    missingRequest.failed && missingRequest.failureType === '0',
    'RM-C5 missing request should fail with public NotFound (0).'
  );
  await postJson(locationConsumerUrl, '/profile/missing-command', { commandId: 'missing-send' });
  const evidence = [
    ...await firstDispatchErrorEvidence(providerAUrl, providerBUrl, 'MissingProfileReq'),
    ...await firstDispatchErrorEvidence(providerAUrl, providerBUrl, 'MissingProfileMsg')
  ];
  ensure(
    evidence.some((line) => line.includes('packet=MissingProfileReq')
      && line.includes('reason=handlerMissing')
      && line.includes('action=replyError')),
    'RM-C5 missing request HandlerMissing/ReplyError evidence missing.'
  );
  ensure(
    evidence.some((line) => line.includes('packet=MissingProfileMsg')
      && line.includes('reason=handlerMissing')
      && line.includes('action=drop')),
    'RM-C5 missing send HandlerMissing/Drop evidence missing.'
  );
  const reply = await postJson<ProfileRes>(locationConsumerUrl, '/profile/request', { value: 'rm-c5-after' });
  ensure(reply.value === 'profile:rm-c5-after', 'RM-C5 normal request after negative path failed.');
  console.log('scenario RM-C5 passed');
}

async function firstDispatchErrorEvidence(providerAUrl: string, providerBUrl: string, packetName: string): Promise<string[]> {
  return await Promise.race([
    postJson<string[]>(providerAUrl, '/evidence/wait', { contains: packetName }),
    postJson<string[]>(providerBUrl, '/evidence/wait', { contains: packetName })
  ]);
}
