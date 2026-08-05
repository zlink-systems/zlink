// TD-E3: 반대 방향 local Join 두 개를 함께 진행한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdE3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdE3();
