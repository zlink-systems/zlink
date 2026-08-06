// SA-E2E-06: Relocate와 Shutdown admission seal을 지킨다 시나리오를 검증한다.
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

export const scenarioId = 'SA-E2E-06' as const;

export async function runSAE2E06(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa06-admission-seal';
  await closeGate(context);
  terminal(await submit(context, context.callerUrl, operationId, context.targetRid), operationId);
  const entered = await waitEvidence(context, operationId, (value) => value.entered === 1);
  ensure(entered.completed === 0, 'SA-E2E-06 operation bypassed the gate');
  await openGate(context);
  await waitEvidence(context, operationId, (value) => value.completed === 1);
  emit(context, { status: 'submitted', admissionGate: true });
}
