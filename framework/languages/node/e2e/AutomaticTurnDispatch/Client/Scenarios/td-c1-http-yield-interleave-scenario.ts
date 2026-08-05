// TD-C1: I/O worker를 Yield로 기다린다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdC1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdC1();
