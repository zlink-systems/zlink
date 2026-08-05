// TD-F4: Timeout 뒤 Spot turn을 반환한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF4 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF4();
