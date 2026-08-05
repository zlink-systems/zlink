import {
  ZLinkFrameworkException,
  type ZLinkRouteClient,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import {
  MissingProfileMsg,
  MissingProfileReq,
  PayloadReq,
  ProfileMsg,
  ProfileReq,
  type PayloadRes,
  type PeerLocationWaitReq,
  type ProfileRes,
  type RequestFailureRes,
  type RequestOutcomeRes
} from '../../../Shared/messages';
import type { HttpRoute } from '../Support/http-server';

export function createConsumerEndpoints(
  channel: ZLinkRouteClient,
  locationQuery: ZLinkLocationRuntimeQuery | undefined,
  routeRuntime: ZLinkRouteMeshRuntime,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready' }) },
    {
      method: 'GET',
      path: '/location/topology',
      handle: () => locationQuery === undefined
        ? Promise.reject(new Error('Location runtime query is not configured.'))
        : listProfileTopology(locationQuery)
    },
    { method: 'POST', path: '/profile/batch-request', handle: (body) => batchRequest(channel, (body as ProfileReq[]).map((request) => new ProfileReq(request.value))) },
    { method: 'POST', path: '/profile/request', handle: (body) => requestProfile(channel, new ProfileReq((body as ProfileReq).value), 5000) },
    { method: 'POST', path: '/profile/request/outcome', handle: (body) => requestProfileOutcome(channel, new ProfileReq((body as ProfileReq).value)) },
    { method: 'POST', path: '/profile/slow-request', handle: (body) => requestProfileFailure(channel, new ProfileReq((body as ProfileReq).value), 100) },
    { method: 'POST', path: '/profile/missing-request', handle: (body) => requestMissingProfile(channel, new MissingProfileReq((body as ProfileReq).value)) },
    {
      method: 'POST',
      path: '/profile/missing-command',
      handle: async (body) => {
        channel
          .sendToChannel('profile', new MissingProfileMsg((body as ProfileMsg).commandId))
          .submit();
        return { status: 'sent' };
      }
    },
    { method: 'POST', path: '/profile/payload', handle: (body) => requestPayload(channel, toPayloadReq(body)) },
    { method: 'POST', path: '/profile/backpressure/reset', handle: () => ({ status: 'ready' }) },
    { method: 'POST', path: '/profile/backpressure/send', handle: (body) => submitProfileUnderPressure(channel, new ProfileMsg((body as ProfileMsg).commandId)) },
    { method: 'GET', path: '/route/status', handle: () => routeRuntime.snapshot('profile') },
    {
      method: 'POST', path: '/locations/peers/wait',
      handle: (body) => locationQuery === undefined
        ? Promise.reject(new Error('Location runtime query is not configured.'))
        : waitForPeer(locationQuery, body as PeerLocationWaitReq)
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

async function requestProfileOutcome(channel: ZLinkRouteClient, request: ProfileReq): Promise<RequestOutcomeRes> {
  try {
    const reply = await requestProfile(channel, request, 5000);
    return { value: request.value, outcome: reply.providerRid };
  } catch (error) {
    const outcome = error instanceof Error && /timed out|timeout/i.test(error.message) ? 'Timeout'
      : error instanceof ZLinkFrameworkException ? String(error.kind)
        : error instanceof Error ? error.name : 'Error';
    return { value: request.value, outcome };
  }
}

async function waitForPeer(
  locationQuery: ZLinkLocationRuntimeQuery,
  request: PeerLocationWaitReq
): Promise<readonly object[]> {
  const deadline = Date.now() + Math.max(1, Math.min(request.timeoutMilliseconds ?? 30_000, 60_000));
  do {
    const rows = await listProfileTopology(locationQuery);
    const present = rows.some((row) => row.routingId === request.rid);
    if (present === request.present) return rows;
    await new Promise((resolve) => setTimeout(resolve, 100));
  } while (Date.now() < deadline);
  throw new Error(`Peer '${request.rid}' did not become present=${request.present}.`);
}

async function listProfileTopology(locationQuery: ZLinkLocationRuntimeQuery): Promise<readonly {
  readonly channelName: string;
  readonly serviceRole: number;
  readonly routingId: string;
  readonly endpoint: string;
  readonly channelWeight?: number;
}[]> {
  const page = await locationQuery.listMeshNodeDescriptors('profile');
  return page.items
    .filter((row) => row.channelWeights.profile !== undefined)
    .map((row) => ({
      channelName: row.meshName,
      // Preserve the legacy endpoint's serialized Router role value.
      serviceRole: 3,
      routingId: String(row.rid),
      endpoint: row.endpoint,
      channelWeight: row.channelWeights.profile
    }));
}

async function batchRequest(channel: ZLinkRouteClient, requests: readonly ProfileReq[]): Promise<ProfileRes[]> {
  const replies: ProfileRes[] = [];
  for (const request of requests) {
    replies.push(await requestProfile(channel, request, 5000));
  }
  return replies;
}

async function requestProfile(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<ProfileRes> {
  return channel
    .requestToChannel('profile', request)
    .timeout(timeoutMs)
    .submit<ProfileRes>();
}

async function requestPayload(channel: ZLinkRouteClient, request: PayloadReq): Promise<PayloadRes> {
  return channel
    .requestToChannel('profile', request)
    .timeout(10000)
    .submit<PayloadRes>();
}

async function requestProfileFailure(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<RequestFailureRes> {
  try {
    await requestProfile(channel, request, timeoutMs);
    return { failed: false, failureType: '' };
  } catch (error) {
    return { failed: true, failureType: publicFailureType(error) };
  }
}

async function requestMissingProfile(channel: ZLinkRouteClient, request: MissingProfileReq): Promise<RequestFailureRes> {
  try {
    await channel
      .requestToChannel('profile', request)
      .timeout(5000)
      .submit<ProfileRes>();
    return { failed: false, failureType: '' };
  } catch (error) {
    return { failed: true, failureType: publicFailureType(error) };
  }
}

function publicFailureType(error: unknown): string {
  return error instanceof ZLinkFrameworkException ? String(error.kind)
    : error instanceof Error ? error.name
      : 'Error';
}

function toPayloadReq(body: unknown): PayloadReq {
  const request = body as PayloadReq;
  return new PayloadReq(request.marker, request.payload);
}

function submitProfileUnderPressure(channel: ZLinkRouteClient, command: ProfileMsg): string {
  channel
    .sendToChannel('profile', command)
    .submit();
  return 'Submitted';
}
