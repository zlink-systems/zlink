// TD-A3: Async 구간의 read-modify-write를 보존한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdA3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdA3();
