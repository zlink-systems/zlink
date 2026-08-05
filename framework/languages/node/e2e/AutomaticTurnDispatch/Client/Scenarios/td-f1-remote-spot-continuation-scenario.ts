// TD-F1: Remote Spot request에서도 Async와 Yield 의미가 같다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF1();
