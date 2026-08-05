import type { ControlPingRes, EvidenceWaitReq } from '../../../Shared/messages';
import { ControlPingReq, SpotServiceNames, spotServicePacket } from '../../../Shared/messages';
import type { ZLinkRouteClient } from '@zlink-systems/framework';
import type { EvidenceStore } from '../Infrastructure/evidence-store';
import type { HttpRoute } from '../Support/http-server';

export function createSessionEndpoints(evidence: EvidenceStore, route: ZLinkRouteClient, stop: () => void): HttpRoute[] {
  const controlPingRoute = (targetRid: string): HttpRoute => ({
    method: 'POST',
    path: `/channel/control-pingMsg/${targetRid}`,
    handle: async (body) => await route
      .requestToNode(SpotServiceNames.controlChannel, targetRid,
        spotServicePacket(ControlPingReq, body as ControlPingReq))
      .timeout(5000)
      .submit<ControlPingRes>()
  });

  return [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ready', role: 'session', rid: evidence.rid }) },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    controlPingRoute('play-a'),
    controlPingRoute('play-b'),
    {
      method: 'POST',
      path: '/evidence/wait',
      handle: (body) => {
        const request = body as EvidenceWaitReq;
        const timeout = Math.max(1, Math.min(request.timeoutMilliseconds ?? 10000, 30000));
        return evidence.waitUntil((entries) =>
          request.containsAll.every((expected) => entries.some((entry) => entry.includes(expected))), timeout);
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}
