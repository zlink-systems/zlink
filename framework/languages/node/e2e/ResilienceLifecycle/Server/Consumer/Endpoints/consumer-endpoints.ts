import type {
  ZLinkLocationRuntimeQuery,
  ZLinkRouteClient
} from '@zlink-systems/framework';
import {
  MissingProfileMsg,
  MissingProfileReq,
  PayloadReq,
  ProfileMsg,
  ProfileReq,
  type PayloadRes,
  type ProfileRes,
  type RequestFailureRes,
  type TimeoutRes
} from '../../../Shared/messages';
import type { HttpRoute } from '../Support/http-server';
import type { EvidenceStore } from '../Infrastructure/evidence-store';

export function createConsumerEndpoints(
  channel: ZLinkRouteClient,
  locationQuery: ZLinkLocationRuntimeQuery,
  evidence: EvidenceStore,
  requestWithNewClient: (request: ProfileReq) => Promise<ProfileRes>,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready' }) },
    {
      method: 'GET',
      path: '/location/peers',
      handle: async () => {
        const page = await locationQuery.listMeshNodeDescriptors('profile');
        return page.items.map((row) => ({
          channelName: row.meshName,
          serviceRole: 'router',
          routingId: String(row.rid),
          rid: String(row.rid),
          endpoint: row.endpoint,
          lifecycleGeneration: row.lifecycleGeneration.toString(),
          descriptorRevision: row.descriptorRevision.toString(),
          channelWeight: row.channelWeights.profile
        }));
      }
    },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    {
      method: 'POST', path: '/evidence/wait', handle: async (body) => {
        const request = body as { readonly contains: string; readonly timeoutMilliseconds?: number };
        return await evidence.waitUntil(
          (entries) => entries.some((entry) => entry.includes(request.contains)),
          Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000))
        );
      }
    },
    { method: 'POST', path: '/profile/batch-request', handle: (body) => batchRequest(channel, (body as ProfileReq[]).map(toProfileReq)) },
    { method: 'POST', path: '/profile/request', handle: (body) => requestProfile(channel, toProfileReq(body), 5000) },
    { method: 'POST', path: '/profile/request/no-retry', handle: (body) => requestProfileOnce(channel, toProfileReq(body), 10000) },
    { method: 'POST', path: '/profile/request/timeout/100', handle: (body) => requestProfileTimeout(channel, toProfileReq(body), 100) },
    { method: 'POST', path: '/profile/request/timeout/10000', handle: (body) => requestProfileTimeout(channel, toProfileReq(body), 10000) },
    { method: 'POST', path: '/profile/request/new-client', handle: (body) => requestWithNewClient(toProfileReq(body)) },
    { method: 'POST', path: '/profile/command', handle: (body) => sendProfile(channel, new ProfileMsg((body as ProfileMsg).commandId)) },
    { method: 'POST', path: '/profile/slow-request', handle: (body) => requestProfileFailure(channel, toProfileReq(body), 100) },
    { method: 'POST', path: '/profile/missing-request', handle: (body) => requestMissingProfile(channel, toMissingProfileReq(body)) },
    {
      method: 'POST',
      path: '/profile/missing-command',
      handle: (body) => {
        channel
          .sendToChannel('profile', new MissingProfileMsg((body as ProfileMsg).commandId))
          .submit();
        return { status: 'sent' };
      }
    },
    { method: 'POST', path: '/profile/payload', handle: (body) => requestPayload(channel, toPayloadReq(body)) },
    { method: 'POST', path: '/profile/backpressure/reset', handle: () => ({ status: 'ready' }) },
    { method: 'POST', path: '/profile/backpressure/send', handle: (body) => submitProfileUnderPressure(channel, new ProfileMsg((body as ProfileMsg).commandId)) },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

async function batchRequest(channel: ZLinkRouteClient, requests: readonly ProfileReq[]): Promise<ProfileRes[]> {
  const replies: ProfileRes[] = [];
  for (const request of requests) {
    replies.push(await requestProfile(channel, request, 5000));
  }
  return replies;
}

export async function requestProfile(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<ProfileRes> {
  return await channel
    .requestToChannel('profile', request)
    .timeout(timeoutMs)
    .submit<ProfileRes>();
}

async function requestProfileOnce(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<ProfileRes> {
  return await channel
    .requestToChannel('profile', request)
    .timeout(timeoutMs)
    .submit<ProfileRes>();
}

async function requestProfileTimeout(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<TimeoutRes> {
  try {
    await channel
      .requestToChannel('profile', request)
      .timeout(timeoutMs)
      .submit<ProfileRes>();
    return { status: 200, timedOut: false };
  } catch {
    return { status: 408, timedOut: true };
  }
}

async function requestPayload(channel: ZLinkRouteClient, request: PayloadReq): Promise<PayloadRes> {
  return await channel
    .requestToChannel('profile', request)
    .timeout(10000)
    .submit<PayloadRes>();
}

function sendProfile(channel: ZLinkRouteClient, command: ProfileMsg): { readonly status: string } {
  channel
    .sendToChannel('profile', command)
    .submit();
  return { status: 'sent' };
}

async function requestProfileFailure(
  channel: ZLinkRouteClient,
  request: ProfileReq,
  timeoutMs: number
): Promise<RequestFailureRes> {
  try {
    await requestProfile(channel, request, timeoutMs);
    return { failed: false, failureType: '', failureMessage: '' };
  } catch (error) {
    return {
      failed: true,
      failureType: error instanceof Error ? error.name : 'Error',
      failureMessage: error instanceof Error ? error.message : String(error)
    };
  }
}

async function requestMissingProfile(channel: ZLinkRouteClient, request: MissingProfileReq): Promise<RequestFailureRes> {
  try {
    await channel
      .requestToChannel('profile', request)
      .timeout(5000)
      .submit<ProfileRes>();
    return { failed: false, failureType: '', failureMessage: '' };
  } catch (error) {
    return {
      failed: true,
      failureType: error instanceof Error ? error.name : 'Error',
      failureMessage: error instanceof Error ? error.message : String(error)
    };
  }
}

function toProfileReq(value: unknown): ProfileReq {
  const request = value as ProfileReq;
  return new ProfileReq(request.value, request.marker);
}

function toMissingProfileReq(value: unknown): MissingProfileReq {
  const request = value as ProfileReq;
  return new MissingProfileReq(request.value, request.marker);
}

function toPayloadReq(value: unknown): PayloadReq {
  const request = value as PayloadReq;
  return new PayloadReq(request.marker, request.payload);
}

function submitProfileUnderPressure(channel: ZLinkRouteClient, command: ProfileMsg): string {
  channel
    .sendToChannel('profile', command)
    .submit();
  return 'Submitted';
}
