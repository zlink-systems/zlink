// RL-D3: Public logging sink에서 dispatch error를 확인한다 시나리오를 검증한다.
import type { RequestFailureRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { waitForProviderEvidenceLine } from '../Support/provider-evidence';
import { profileReq, waitForAnyProviderTraffic } from '../Support/resilience-helpers';
import { ensure } from '../Support/scenario-assert';

export async function runRlD3(options: ClientOptions): Promise<void> {
  await waitForAnyProviderTraffic(options.consumerUrl, 'rl-d3-ready');
  const failed = await postJson<RequestFailureRes>(
    options.consumerUrl,
    '/profile/missing-request',
    profileReq('rl-d3-missing')
  );
  ensure(failed.failed, 'RL-D3 expected missing request handler failure.');
  ensure(
    failed.failureMessage.includes('request handler is registered'),
    `RL-D3 request did not reach the remote no-handler path: ${failed.failureMessage}`
  );

  const line = await waitForProviderEvidenceLine(
    options,
    (entry) => entry.includes('dispatch-error|') && entry.includes('packet_name=MissingProfileReq'),
    'RL-D3 dispatch-error evidence timed out.'
  );
  ensure(
    line.includes('outcome=failed')
      && line.includes('reason=no_handler')
      && line.includes('action=reply_error')
      && line.includes('packet_name=MissingProfileReq'),
    'RL-D3 dispatch-error evidence did not preserve the common failure fields.'
  );

  console.log('scenario RL-D3 passed');
}
