import { PerfClock } from '../shared/clock';
import { EndpointManifest, PerfTriggerReply, PerfTriggerRequest, PerfValidationException, ResetReply, ResetRequest, toJson } from '../shared/contracts';

// Only the standalone application client sends phase triggers. HTTP acknowledgements are not echo operations.
export class MetricsClient {
  constructor(private readonly manifest: EndpointManifest) {}

  private async post<T>(url: string, body: unknown, label: string): Promise<T> {
    const response = await fetch(url, { method: 'POST', headers: { 'content-type': 'application/json' }, body: toJson(body),
      signal: AbortSignal.timeout(this.manifest.workload.adminTimeoutMs) });
    const text = await response.text();
    if (!response.ok) throw new Error(`${label}: HTTP ${response.status}: ${text}`);
    return JSON.parse(text) as T;
  }

  async triggerRoles(request: PerfTriggerRequest): Promise<unknown[]> {
    const acknowledgements: unknown[] = [];
    // Manifest order is receivers then source, fixed by the coordinator before processes start.
    for (const role of this.manifest.roles) {
      const sent = PerfClock.now();
      const ack = await this.post<PerfTriggerReply>(role.applicationTriggerUrl, request, `Trigger ${role.role}/${role.roleInstance}`);
      const received = PerfClock.now();
      if (!ack.accepted || ack.runId !== request.runId || ack.cellId !== request.cellId || ack.resetSeq !== request.resetSeq ||
        ack.phase !== request.phase || ack.configHash !== this.manifest.configHash)
        throw new PerfValidationException('PhaseMismatch', 'Role trigger acknowledgement identity differs.');
      acknowledgements.push({ role: role.role, roleInstance: role.roleInstance, sentTicks: sent.toString(), ackTicks: received.toString(),
        clockDomainId: PerfClock.domain, acknowledgement: ack });
    }
    return acknowledgements;
  }

  async resetRoles(request: ResetRequest): Promise<ResetReply[]> {
    const acknowledgements: ResetReply[] = [];
    for (const role of this.manifest.roles) {
      const ack = await this.post<ResetReply>(`${role.metrics.baseUrl}/perf/reset`, request, `Reset ${role.role}/${role.roleInstance}`);
      if (!ack.ok || ack.runId !== request.runId || ack.cellId !== request.cellId || ack.resetSeq !== request.resetSeq)
        throw new PerfValidationException('PhaseMismatch', 'Role reset acknowledgement identity differs.');
      acknowledgements.push(ack);
    }
    return acknowledgements;
  }
}
