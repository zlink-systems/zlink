// SA-E2E-02: HWM이 회복되면 pending send를 수락한다 시나리오를 검증한다.
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

export const scenarioId = 'SA-E2E-02' as const;

export async function runSAE2E02(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa02-hwm-recovery';
  await closeGate(context);
  terminal(await submit(context, context.callerUrl, operationId, context.targetRid), operationId);
  const entered = await waitEvidence(context, operationId, (value) => value.entered === 1);
  ensure(entered.completed === 0, 'SA-E2E-02 handler completed while the gate was closed');
  await openGate(context);
  const completed = await waitEvidence(context, operationId, (value) => value.completed === 1);
  ensure(completed.entered === 1, 'SA-E2E-02 handler count mismatch');
  emit(context, { status: 'submitted', gateRecovery: true, handlerCount: 1 });
}
