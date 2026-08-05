// OBS-B3: Publish metric 부재와 owner lease lateness를 확인한다 시나리오를 검증한다.
import { post, require, unique, workflowA, workflowB } from '../Support/scenario-support.js';
import { metric, metrics, waitFor } from '../Support/observability-support.js';
import type { WorkflowApplyRes } from '../../Shared/messages.js';

export async function runObsB3(): Promise<void> {
  const orderId = unique('obs-b3-order');
  await post<WorkflowApplyRes>(workflowA, '/workflows', { orderId, value: 1 });
  const lease = await waitFor(async () => [...await metrics(workflowA), ...await metrics(workflowB)],
    (values) => values.some((value) => value.name === 'zlink.location.owner_lease.renew.lateness'),
    'OBS-B3 owner lease lateness metric missing');
  const all = lease;
  const forbidden = ['correlation_id', 'flow_id', 'actor_id', 'spot_id'];
  require(all.every((value) => forbidden.every((label) => !(label in value.tags))),
    'OBS-B3 emitted a high-cardinality metric label.');
  require(!all.some((value) => value.name === 'zlink.fanout.dropped' && value.value === 0),
    'OBS-B3 emitted an unsupported zero fanout.dropped instrument.');
  require(!all.some((value) => value.name.startsWith('zlink.fanout.')),
    'OBS-B3 emitted a publish-specific fanout metric.');
  metric(all, 'zlink.location.owner_lease.renew.lateness');
}
