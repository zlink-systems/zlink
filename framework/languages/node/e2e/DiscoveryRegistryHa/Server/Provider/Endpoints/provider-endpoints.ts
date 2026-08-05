import {
  ZLinkFrameworkRelocationMode,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime
} from '@zlink-systems/framework';
import { ChannelNames, type EvidenceWaitReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createProviderEndpoints(
  evidence: EvidenceStore,
  runtimeOptions: ZLinkRouteMeshRuntimeOptions,
  frameworkRuntime: ZLinkFrameworkRuntime,
  stop: () => void
): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'provider', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        return evidence.waitUntil(
          (entries) => entries.some((entry) => entry.includes(request.contains)),
          Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000))
        );
      }
    },
    {
      method: 'POST',
      path: '/drain',
      handle: async () => {
        runtimeOptions.channel(ChannelNames.profile).weight = 0;
        evidence.add(`drain-started|rid=${evidence.rid}|weight=0`);
        const result = await frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance, deadlineMs: 30_000 });
        evidence.add(`retire-finished|rid=${evidence.rid}|outcome=${result.outcome}|reason=${result.reason}`);
        stop();
        return result;
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}
