import type { ZLinkRouteClient } from '@zlink-systems/framework';
import { WorkflowReq, type EvidenceWaitReq, type WorkflowRes } from '../../../Shared/messages';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createWorkflowEndpoints(
  evidence: EvidenceStore,
  channel: ZLinkRouteClient,
  stop: () => void
): HttpRoute[] {
  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'workflow', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    { method: 'POST', path: '/workflow/request', handle: (body) => requestWorkflow(channel, new WorkflowReq((body as WorkflowReq).value)) },
    { method: 'POST', path: '/evidence/clear', handle: () => { evidence.clear(); return { status: 'cleared' }; } },
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        return evidence.waitUntil((entries) => entries.some((line) => line.includes(request.contains)), timeout);
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

async function requestWorkflow(
  channel: ZLinkRouteClient,
  request: WorkflowReq
): Promise<WorkflowRes> {
  return channel
    .requestToChannel('workflow', request)
    .timeout(5000)
    .submit<WorkflowRes>();
}
