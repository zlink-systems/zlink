import type {
  ZLinkFanoutClient,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntimeOptions
} from '@zlink-systems/framework';
import {
  LoadEvent,
  ProfileMsg,
  ProfileReq,
  ResilienceNames,
  type EvidenceWaitReq,
  type ProfileRes,
  type WeightWaitReq,
} from '../../../Shared/messages';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import type { FaultState } from '../Infrastructure/fault-state';
import type { HttpRoute } from '../Support/http-server';

export function createProviderEndpoints(
  evidence: EvidenceStore,
  fault: FaultState,
  channel: ZLinkRouteClient,
  fanout: ZLinkFanoutClient,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'provider', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/fanout/publish',
      handle: async (body) => {
        const event = body as LoadEvent;
        await fanout.publish(
          ResilienceNames.fanoutChannel,
          new LoadEvent(event.runId, Number(event.sequence))
        ).submit();
        return { status: 'published', sequence: event.sequence };
      }
    },
    {
      method: 'POST',
      path: '/profile/request',
      handle: (body) => requestProfile(channel, 'profile', new ProfileReq((body as ProfileReq).value, (body as ProfileReq).marker))
    },
    {
      method: 'POST',
      path: '/profile/command',
      handle: async (body) => {
        await sendProfile(channel, 'profile', new ProfileMsg((body as ProfileMsg).commandId));
        return { status: 'sent' };
      }
    },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    {
      method: 'POST',
      path: '/admin/fault/gray',
      handle: () => {
        fault.mode = 'gray';
        evidence.add(`admin|rid=${evidence.rid}|action=fault|mode=gray`);
        return { status: 'fault', mode: fault.mode };
      }
    },
    {
      method: 'POST',
      path: '/admin/fault/none',
      handle: () => {
        fault.mode = 'none';
        evidence.add(`admin|rid=${evidence.rid}|action=fault|mode=none`);
        return { status: 'fault', mode: fault.mode };
      }
    },
    {
      method: 'POST',
      path: '/admin/fault/observer-throws',
      handle: () => {
        fault.mode = 'observer-throws';
        evidence.add(`admin|rid=${evidence.rid}|action=fault|mode=observer-throws`);
        return { status: 'fault', mode: fault.mode };
      }
    },
    {
      method: 'POST',
      path: '/admin/fault/crash-on-slow-start',
      handle: () => {
        fault.mode = 'crash-on-slow-start';
        evidence.add(`admin|rid=${evidence.rid}|action=fault|mode=crash-on-slow-start`);
        return { status: 'fault', mode: fault.mode };
      }
    },
    {
      method: 'POST',
      path: '/admin/crash',
      handle: () => {
        setTimeout(() => process.kill(process.pid, 'SIGKILL'), 50);
        return { status: 'crashing' };
      }
    },
    {
      method: 'POST',
      path: '/admin/drain',
      handle: () => {
        runtimeOptions.channel('profile').weight = 0;
        evidence.add(`admin|rid=${evidence.rid}|action=drain|weight=0`);
        return { status: 'drained', weight: 0 };
      }
    },
    {
      method: 'POST',
      path: '/admin/restore',
      handle: () => {
        runtimeOptions.channel('profile').weight = 100;
        evidence.add(`admin|rid=${evidence.rid}|action=restore|weight=100`);
        return { status: 'restored', weight: 100 };
      }
    },
    {
      method: 'GET',
      path: '/admin/weight',
      handle: () => ({
        weight: runtimeOptions.channel('profile').weight
      })
    },
    {
      method: 'POST',
      path: '/admin/weight/wait',
      handle: async (body) => waitForWeight(runtimeOptions, body as WeightWaitReq)
    },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: async (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        const snapshot = await evidence.waitUntil(
          (entries) => entries.some((line) => line.includes(request.contains)),
          timeout
        );
        if (!snapshot.some((line) => line.includes(request.contains))) {
          throw new Error(`Evidence marker not found: ${request.contains}`);
        }
        return snapshot;
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

async function waitForWeight(
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  request: WeightWaitReq
): Promise<{ readonly weight: number }> {
  const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const weight = runtimeOptions.channel('profile').weight;
    if (weight === request.expected) {
      return { weight };
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Provider weight did not become ${request.expected}.`);
}

async function requestProfile(
  channel: ZLinkRouteClient,
  channelName: string,
  request: ProfileReq
): Promise<ProfileRes> {
  return await channel
    .requestToChannel(channelName, request)
    .timeout(5000)
    .submit<ProfileRes>();
}

async function sendProfile(
  channel: ZLinkRouteClient,
  channelName: string,
  command: ProfileMsg
): Promise<void> {
  await channel
    .sendToChannel(channelName, command)
    .submit();
}
