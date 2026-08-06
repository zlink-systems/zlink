// SA-E2E-18: Direct target과 Channel select-one을 구분한다 시나리오를 검증한다.
import {
  emit,
  submit,
  submitChannel,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-18' as const;

export async function runSAE2E18(context: SubmitScenarioContext): Promise<void> {
  const directId = 'sa18-direct';
  const channelId = 'sa18-channel';
  terminal(await submit(context, context.callerUrl, directId, context.targetRid), directId);
  terminal(await submitChannel(context, channelId), channelId);
  await waitEvidence(context, directId, (value) => value.completed === 1);
  await waitEvidence(context, channelId, (value) => value.completed === 1);
  emit(context, { status: 'submitted', direct: true, channel: true });
}
