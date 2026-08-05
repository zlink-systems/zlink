// TD-F6: Wait-for cycle을 timeout 전에 거부한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF6 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF6();
