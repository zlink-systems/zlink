// TD-D1: SpotWide Actor가 Yield하면 다른 Actor와 Spot callback이 진행한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdD1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD1();
