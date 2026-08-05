// TD-D4: PerActor Async는 같은 Actor lane만 막는다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';

export const runTdD4 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD4();
