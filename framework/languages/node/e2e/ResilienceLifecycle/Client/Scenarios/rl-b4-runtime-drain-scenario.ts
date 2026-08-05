// RL-B4: Runtime weight 0으로 신규 selection에서 제외하고 복원한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { profileReq, waitForAnyProviderTraffic } from '../Support/resilience-helpers';
import { countNewEvidence, ensure } from '../Support/scenario-assert';

interface MeshDescriptorEvidence {
  readonly rid: string;
  readonly endpoint: string;
  readonly lifecycleGeneration: string;
  readonly descriptorRevision: string;
  readonly channelWeight?: number;
}

export async function runRlB4(options: ClientOptions): Promise<void> {
  await waitForAnyProviderTraffic(options.consumerUrl, 'rl-b4-ready');
  await postJson(options.providerAUrl, '/admin/restore');
  await postJson(options.providerBUrl, '/admin/restore');
  await waitForWeight(options.providerAUrl, 100);
  await waitForWeight(options.providerBUrl, 100);

  const beforeDescriptor = await waitForPublishedWeight(options, 'api-b', 100);
  const apiABefore = await waitForPublishedWeight(options, 'api-a', 100);
  await postJson(options.providerAUrl, '/admin/drain');
  await waitForWeight(options.providerAUrl, 0);
  await waitForPublishedWeight(
    options,
    'api-a',
    0,
    BigInt(apiABefore.descriptorRevision)
  );
  const accepted = await startAcceptedWorkOnProviderB(options);
  await postJson(options.providerAUrl, '/admin/restore');
  await waitForWeight(options.providerAUrl, 100);
  await waitForPublishedWeight(options, 'api-a', 100);
  const beforeDrain = await getJson<string[]>(options.providerBUrl, '/evidence');

  await postJson(options.providerBUrl, '/admin/drain');
  await waitForWeight(options.providerBUrl, 0);
  const drainedDescriptor = await waitForPublishedWeight(
    options,
    'api-b',
    0,
    BigInt(beforeDescriptor.descriptorRevision)
  );
  ensure(
    drainedDescriptor.endpoint === beforeDescriptor.endpoint
      && drainedDescriptor.lifecycleGeneration === beforeDescriptor.lifecycleGeneration,
    'RL-B4 weight 0 changed provider identity or endpoint.'
  );

  for (let i = 0; i < 20; i += 1) {
    const marker = `rl-b4-drained-${i}`;
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', profileReq(marker));
    ensure(reply.providerRid === 'api-a', 'RL-B4 drained api-b received a new request.');
  }

  const afterDrain = await getJson<string[]>(options.providerBUrl, '/evidence');
  ensure(
    countNewEvidence(afterDrain, beforeDrain, 'profile-request|rid=api-b', 'marker=rl-b4-drained-') === 0,
    'RL-B4 api-b evidence changed after drain.'
  );
  const acceptedReply = await accepted.task;
  ensure(
    acceptedReply.providerRid === 'api-b',
    'RL-B4 weight 0 canceled work accepted before the descriptor update.'
  );
  const afterAccepted = await getJson<string[]>(options.providerBUrl, '/evidence');
  ensure(
    afterAccepted.some((line) =>
      line.includes(`profile-request|rid=api-b|value=accepted|marker=${accepted.marker}`)),
    'RL-B4 accepted work completion evidence is missing.'
  );
  await postJson<string[]>(options.providerAUrl, '/evidence/wait', {
    contains: 'profile-request|rid=api-a|value=fast|marker=rl-b4-drained-'
  });

  await postJson(options.providerBUrl, '/admin/restore');
  await waitForWeight(options.providerBUrl, 100);
  const restoredDescriptor = await waitForPublishedWeight(
    options,
    'api-b',
    100,
    BigInt(drainedDescriptor.descriptorRevision)
  );
  ensure(
    restoredDescriptor.endpoint === beforeDescriptor.endpoint
      && restoredDescriptor.lifecycleGeneration === beforeDescriptor.lifecycleGeneration,
    'RL-B4 weight restoration changed provider identity or endpoint.'
  );

  let restoredApiB = false;
  for (let i = 0; i < 240; i += 1) {
    const reply = await postJson<ProfileRes>(
      options.consumerUrl,
      '/profile/request',
      profileReq(`rl-b4-restored-${i}`)
    );
    ensure(reply.value === 'profile:fast', 'RL-B4 restored request returned an unexpected value.');
    if (reply.providerRid === 'api-b') {
      restoredApiB = true;
      break;
    }
  }
  ensure(restoredApiB, 'RL-B4 restored api-b did not re-enter selection.');

  await postJson<string[]>(options.providerBUrl, '/evidence/wait', {
    contains: 'profile-request|rid=api-b|value=fast|marker=rl-b4-restored-'
  });
  console.log('scenario RL-B4 passed');
}

async function waitForWeight(providerUrl: string, expected: number): Promise<void> {
  await postJson(providerUrl, '/admin/weight/wait', { expected });
}

async function waitForPublishedWeight(
  options: ClientOptions,
  rid: string,
  expectedWeight: number,
  afterRevision?: bigint
): Promise<MeshDescriptorEvidence> {
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const descriptors = await getJson<MeshDescriptorEvidence[]>(
      options.peerLocationUrl,
      '/location/peers'
    );
    const descriptor = descriptors.find((entry) => entry.rid === rid);
    if (
      descriptor !== undefined
      && descriptor.channelWeight === expectedWeight
      && (
        afterRevision === undefined
        || BigInt(descriptor.descriptorRevision) > afterRevision
      )
    ) {
      return descriptor;
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw new Error(
    `RL-B4 ${rid} descriptor did not publish weight=${expectedWeight}`
    + `${afterRevision === undefined ? '' : ` after revision=${afterRevision}`}.`
  );
}

async function startAcceptedWorkOnProviderB(
  options: ClientOptions
): Promise<{ readonly marker: string; readonly task: Promise<ProfileRes> }> {
  const marker = `rl-b4-accepted-${Date.now()}`;
  const task = postJson<ProfileRes>(
    options.consumerUrl,
    '/profile/request/no-retry',
    { value: 'accepted', marker }
  );
  const provider = await waitForAcceptedStart(options, marker);
  ensure(provider === 'api-b', 'RL-B4 accepted work did not start on api-b.');
  return { marker, task };
}

async function waitForAcceptedStart(
  options: ClientOptions,
  marker: string
): Promise<'api-a' | 'api-b'> {
  const deadline = Date.now() + 15000;
  while (Date.now() < deadline) {
    const [apiA, apiB] = await Promise.all([
      getJson<string[]>(options.providerAUrl, '/evidence'),
      getJson<string[]>(options.providerBUrl, '/evidence')
    ]);
    if (apiA.some((line) => line.includes(`profile-start|rid=api-a|value=accepted|marker=${marker}`))) {
      return 'api-a';
    }
    if (apiB.some((line) => line.includes(`profile-start|rid=api-b|value=accepted|marker=${marker}`))) {
      return 'api-b';
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`RL-B4 accepted request did not start: marker=${marker}.`);
}
