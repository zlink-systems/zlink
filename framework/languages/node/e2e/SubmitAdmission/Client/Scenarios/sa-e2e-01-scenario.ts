// SA-E2E-01: Ready target에 즉시 submit한다 시나리오를 검증한다.
import {
  emit,
  ensure,
  submit,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-01' as const;

export async function runSAE2E01(context: SubmitScenarioContext): Promise<void> {
  const nodeId = 'sa01-node';
  const channelId = 'sa01-channel';
  terminal(await submit(context, context.callerUrl, nodeId, context.targetRid), nodeId);
  terminal(await submitChannel(context, channelId), channelId);
  const nodeEvidence = await waitEvidence(context, nodeId, (value) => value.completed === 1);
  const channelEvidence = await waitEvidence(context, channelId, (value) => value.completed === 1);
  ensure(nodeEvidence.entered === 1 && channelEvidence.entered === 1, 'SA-E2E-01 handler count mismatch');
  emit(context, { families: ['node-direct', 'channel-name'], handlerCount: 1 });
}
