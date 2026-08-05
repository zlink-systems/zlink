// TD-B2: Yield continuation은 기존 queue 순서를 따른다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdB2 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdB2();
