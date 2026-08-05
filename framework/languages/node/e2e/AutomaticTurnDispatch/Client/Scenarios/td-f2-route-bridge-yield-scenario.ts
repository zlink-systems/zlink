// TD-F2: Channel handler에서 시작해도 같은 의미를 사용한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF2 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF2();
