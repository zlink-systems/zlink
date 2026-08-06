// SA-E2E-04: Deadline 뒤 늦은 capacity가 operation을 되살리지 않는다 시나리오를 검증한다.
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

export const scenarioId = 'SA-E2E-04' as const;

export async function runSAE2E04(context: SubmitScenarioContext): Promise<void> {
  const operationId = 'sa04-deadline-seal';
  await closeGate(context);
  terminal(await submit(context, context.callerUrl, operationId, context.targetRid), operationId);
  const blocked = await waitEvidence(context, operationId, (value) => value.entered === 1);
  ensure(blocked.completed === 0, 'SA-E2E-04 operation bypassed the admission gate');
  await openGate(context);
  await waitEvidence(context, operationId, (value) => value.completed === 1);
  emit(context, { status: 'submitted', terminalBeforeGateRelease: true });
}
