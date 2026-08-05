// SM-G4: 많은 bound Session pushes를 target별로 격리한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type { ZlinkStreamMessage } from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPushNotify,
  ActorPushReq,
  AuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

interface BoundClient {
  readonly actorId: string;
  readonly value: string;
  readonly client: ReturnType<typeof createStreamClient>;
  crossDelivered: number;
}

export async function runSmG4(options: ClientOptions): Promise<void> {
  const sessionCount = 6;
  const clients: BoundClient[] = [];
  const subscriptions: Array<{ dispose(): void }> = [];

  try {
    for (let index = 0; index < sessionCount; index += 1) {
      const actorId = `actor-sm-g4-${index}`;
      const value = `push-${index}`;
      const client = createStreamClient(options.sessionAStreamEndpoint);
      const entry: BoundClient = {
        actorId,
        value,
        client,
        crossDelivered: 0
      };
      clients.push(entry);
      subscriptions.push(client.on<ActorPushNotify>('ActorPushNotify', (message) => {
        if (message.payload.actorId !== actorId) {
          entry.crossDelivered += 1;
        }
      }));

      await client.connect();
      await client
        .request({
          actorId,
          displayName: `bound-load-${index}`,
          nodeRid: 'play-a'
        } satisfies AuthReq)
        .packetName('AuthReq')
        .timeout(5000)
        .submit();
    }

    const results: Array<{
      readonly entry: BoundClient;
      readonly reply: ActorPingRes;
      readonly notify: ZlinkStreamMessage<ActorPushNotify>;
    }> = [];
    for (const entry of clients) {
      const pushed = entry.client.waitFor<ActorPushNotify>('ActorPushNotify')
        .where((message) => message.payload.actorId === entry.actorId)
        .timeout(10000)
        .submit();
      const reply = await entry.client
        .request({ value: entry.value } satisfies ActorPushReq)
        .packetName('ActorPushReq')
        .timeout(5000)
        .submit<ActorPingRes>();
      const notify = await pushed;
      results.push({ entry, reply, notify });
    }

    for (const { entry, reply, notify } of results) {
      ensure(reply.actorId === entry.actorId, 'SM-G4 push reply actor mismatch.');
      ensure(reply.value === entry.value, 'SM-G4 push reply value mismatch.');
      ensure(notify.payload.actorId === entry.actorId, 'SM-G4 push notify actor mismatch.');
      ensure(notify.payload.value === entry.value, 'SM-G4 push notify value mismatch.');
    }

    await new Promise((resolve) => setTimeout(resolve, 200));
    for (const entry of clients) {
      ensure(entry.crossDelivered === 0, `SM-G4 cross-delivered push for ${entry.actorId}.`);
    }
  } finally {
    for (const subscription of subscriptions) {
      subscription.dispose();
    }
    await Promise.allSettled(clients.map((entry) => entry.client.close()));
  }

  console.log('scenario SM-G4 passed');
}

function createStreamClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
}
