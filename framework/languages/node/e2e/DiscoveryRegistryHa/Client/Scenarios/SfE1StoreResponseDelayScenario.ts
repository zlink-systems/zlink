// SF-E1: Store response가 대기 중이어도 무관한 request를 처리한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJsonWithin, postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface StoreResponseGateStatus {
  readonly open: boolean;
  readonly waiting: number;
}

export async function runSfE1(options: ClientOptions): Promise<void> {
  await postJsonWithin<StoreResponseGateStatus>(options.consumerUrl, '/location/store-gate/close', {}, 1200);
  const delayedPeerRead = getJsonWithin<Array<{ nodeRid: string }>>(options.consumerUrl, '/location/peers', 6000);
  try {
    const gateDeadline = Date.now() + 3000;
    let held = false;
    while (Date.now() < gateDeadline) {
      const status = await getJsonWithin<StoreResponseGateStatus>(options.consumerUrl, '/location/store-gate', 1200);
      if (!status.open && status.waiting > 0) {
        held = true;
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    ensure(held, 'SF-E1 Store response gate did not hold the peer query.');

    const replies = await Promise.all(Array.from({ length: 100 }, (_, index) =>
      postJsonWithin<ProfileRes>(
        options.consumerUrl,
        '/profile/request-once',
        { value: `sf-e1-${index}` },
        1200
      )
    ));
    replies.forEach((reply, index) => {
      ensure(reply.value === `profile:sf-e1-${index}`, `SF-E1 reply ${index} value mismatch.`);
      ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', `SF-E1 reply ${index} provider mismatch.`);
    });
  } finally {
    await postJsonWithin<StoreResponseGateStatus>(options.consumerUrl, '/location/store-gate/open', {}, 1200);
  }

  const peers = await delayedPeerRead;
  ensure(peers.some((peer) => peer.nodeRid === 'api-a'), 'SF-E1 delayed peer read did not recover api-a.');
  ensure(peers.some((peer) => peer.nodeRid === 'api-b'), 'SF-E1 delayed peer read did not recover api-b.');
  console.log('scenario SF-E1 passed');
}
