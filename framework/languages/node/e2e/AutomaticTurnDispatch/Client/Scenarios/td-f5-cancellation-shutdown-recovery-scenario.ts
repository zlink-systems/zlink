// TD-F5: Waiter cancellation 뒤 owner를 계속 사용한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF5 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF5();
