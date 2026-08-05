// RM-B3: Provider crash 뒤 남은 provider를 사용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { DynamicClusterLauncher } from '../Support/dynamic-cluster-launcher';
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import { ensure, uniqueMarker } from '../Support/scenario-assert';
import type { ProfileRes, RequestOutcomeRes, RouteMissingRes } from '../../Shared/messages';

const ROUTE_NOT_CONNECTED = '5';
const REQUEST_TARGET_NOT_FOUND = '0';

export async function runRmB3(options: ClientOptions): Promise<void> {
  const cluster = await DynamicClusterLauncher.start(options, 'rm-b3');
  try {
    const providerA = await cluster.startProvider('rm-b3-api-a', 'api-a');
    const providerB = await cluster.startProvider('rm-b3-api-b', 'api-b', 100, [providerA.routeEndpoint]);
    const consumer = await cluster.startConsumer('rm-b3-consumer');
    await cluster.waitForProviders([providerA.channelEndpoint, providerB.channelEndpoint]);
    await proveBothProviders(consumer.httpUrl);

    const transitionMarker = uniqueMarker('rm-b3-transition');
    const inFlight = Array.from({ length: 4 }, (_, index) => {
      const value = `${transitionMarker}-${index}`;
      return { value, task: observeRequest(consumer.httpUrl, value) };
    });
    const startedEvidence = await postJson<string[]>(providerA.httpUrl, '/evidence/wait', {
      contains: `profile-request-start|rid=api-a|value=${transitionMarker}`,
      timeoutMilliseconds: 10_000
    });
    const startedLine = startedEvidence.find((line) => line.includes(`profile-request-start|rid=api-a|value=${transitionMarker}`));
    ensure(startedLine !== undefined, 'RM-B3 did not observe an in-flight request on api-a before crash.');
    const startedValue = startedLine.slice(startedLine.indexOf('|value=') + '|value='.length);

    await cluster.crash(providerA);
    await postJson(consumer.httpUrl, '/locations/peers/wait', {
      rid: 'api-a', present: true, timeoutMilliseconds: 1_000
    });
    const continuingMarker = uniqueMarker('rm-b3-continuing');
    const continuing = await Promise.all(Array.from({ length: 20 }, (_, index) =>
      observeRequest(consumer.httpUrl, `${continuingMarker}-${index}`)));
    const transition = await Promise.all(inFlight.map((request) => request.task));
    const crashedRequest = transition.find((result) => result.value === startedValue);
    ensure(
      crashedRequest !== undefined
        && (crashedRequest.outcome === ROUTE_NOT_CONNECTED || crashedRequest.outcome === 'Timeout'),
      `RM-B3 request started on crashed api-a completed as '${crashedRequest?.outcome ?? 'missing'}'.`
    );
    ensure(
      transition.every((result) => isTransitionOutcome(result.outcome)),
      'RM-B3 in-flight request completed outside the bounded public outcome set.'
    );
    ensure(continuing.some((result) => result.outcome === 'api-b'), 'RM-B3 remaining provider served no request after crash.');
    ensure(
      continuing.every((result) => result.outcome === 'api-b' || result.outcome === ROUTE_NOT_CONNECTED || result.outcome === 'Timeout'),
      'RM-B3 crash propagation request completed outside the public outcome set.'
    );

    await postJsonWithin(consumer.httpUrl, '/locations/peers/wait', {
      rid: 'api-a', present: false, timeoutMilliseconds: 60_000
    }, 61_000);
    await cluster.waitForSingleProvider('api-b', providerB.channelEndpoint);
    for (let index = 0; index < 20; index += 1) {
      const reply = await postJson<ProfileRes>(consumer.httpUrl, '/profile/request', { value: `rm-b3-after-${index}` });
      ensure(reply.providerRid === 'api-b', 'RM-B3 post-expiry request did not use api-b.');
    }

    const known = await postJsonWithin<RouteMissingRes>(providerB.httpUrl, '/profile/route/target', {
      targetRid: 'api-a', value: 'rm-b3-known-disconnected'
    }, 3_000);
    ensure(
      known.errorKind === ROUTE_NOT_CONNECTED,
      `RM-B3 known disconnected target reported '${known.errorKind}' instead of RouteNotConnected.`
    );
    const missing = await postJson<RouteMissingRes>(providerB.httpUrl, '/profile/route/target', {
      targetRid: 'api-missing', value: 'rm-b3-missing'
    });
    ensure(missing.errorKind === REQUEST_TARGET_NOT_FOUND, 'RM-B3 unknown target did not report RequestTargetNotFound.');
    console.log('scenario RM-B3 passed');
  } finally {
    await cluster.close();
  }
}

async function proveBothProviders(consumerUrl: string): Promise<void> {
  const seen = new Set<string>();
  for (let index = 0; index < 40; index += 1) {
    const reply = await postJson<ProfileRes>(consumerUrl, '/profile/request', { value: `rm-b3-before-${index}` });
    seen.add(reply.providerRid);
  }
  ensure(seen.has('api-a') && seen.has('api-b'), 'RM-B3 expected both providers before crash.');
}

async function observeRequest(consumerUrl: string, value: string): Promise<RequestOutcomeRes> {
  return await postJsonWithin<RequestOutcomeRes>(consumerUrl, '/profile/request/outcome', { value }, 10_000);
}

function isTransitionOutcome(outcome: string): boolean {
  return outcome === 'api-a' || outcome === 'api-b' || outcome === ROUTE_NOT_CONNECTED || outcome === 'Timeout';
}
