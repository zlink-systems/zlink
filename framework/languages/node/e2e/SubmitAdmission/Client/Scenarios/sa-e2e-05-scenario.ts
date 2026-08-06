// SA-E2E-05: Target 부재와 route 미연결을 구분한다 시나리오를 검증한다.
import {
  emit,
  shutdownTarget,
  submit,
  terminal,
  waitRouteState,
  type SubmitScenarioContext
} from '../Support/scenario-http';

export const scenarioId = 'SA-E2E-05' as const;

export async function runSAE2E05(context: SubmitScenarioContext): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const operationId = `sa05-missing-${attempt}`;
    terminal(await submit(context, context.callerUrl, operationId, 'submit-missing'), operationId, 'targetNotFound');
  }
  await shutdownTarget(context);
  await waitRouteState(context, false);
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const operationId = `sa05-disconnected-${attempt}`;
    terminal(await submit(context, context.callerUrl, operationId, context.targetRid), operationId, 'routeNotConnected');
  }
  emit(context, { missingAttempts: 100, disconnectedAttempts: 100, handlerCount: 0 });
}
