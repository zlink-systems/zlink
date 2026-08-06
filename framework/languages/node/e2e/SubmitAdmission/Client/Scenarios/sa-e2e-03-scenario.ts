// SA-E2E-03: Pending send를 bounded terminal로 끝낸다 시나리오를 검증한다.
import {
  emit,
  submit,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-03' as const;

export async function runSAE2E03(context: SubmitScenarioContext): Promise<void> {
  const nodeId = 'sa03-node';
  const channelId = 'sa03-channel';
  terminal(await submit(context, context.callerUrl, nodeId, context.targetRid), nodeId);
  terminal(await submitChannel(context, channelId), channelId);
  await waitEvidence(context, nodeId, (value) => value.completed === 1);
  await waitEvidence(context, channelId, (value) => value.completed === 1);
  emit(context, { status: 'submitted', families: ['node-direct', 'channel-name'] });
}
