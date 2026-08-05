import type { EvidenceWaitReq } from '../../../Shared/messages';
import { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createOperationalEndpoints(evidence: EvidenceStore, stop: () => void): readonly HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'json-only-peer', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: async (body) => {
        const request = body as EvidenceWaitReq;
        return await evidence.waitUntil(request.containsAll, request.timeoutMilliseconds ?? 10_000);
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}
