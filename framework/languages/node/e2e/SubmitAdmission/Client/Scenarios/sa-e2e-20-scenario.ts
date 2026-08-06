// SA-E2E-20: Submit 완료와 remote handler 완료를 분리한다 시나리오를 검증한다.
import {
  closeGate,
  emit,
  ensure,
  openGate,
  submit,
  terminal,
  waitEvidence,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-20' as const;

export async function runSAE2E20(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa20-handler-gate';
  await closeGate(context);
  terminal(await submit(context, context.callerUrl, operationId, context.targetRid), operationId);
  const entered = await waitEvidence(context, operationId, (value) => value.entered === 1);
  ensure(entered.completed === 0, 'SA-E2E-20 handler completed before gate release');
  await openGate(context);
  const completed = await waitEvidence(context, operationId, (value) => value.completed === 1);
  ensure(completed.entered === 1 && completed.completed === 1, 'SA-E2E-20 handler count mismatch');
  emit(context, { status: 'submitted', handlerEnteredBeforeRelease: true });
}
