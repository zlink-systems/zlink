// TD-F5A: Await 중 Host Shutdown을 시작한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';

export const runTdF5A = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF5A();
