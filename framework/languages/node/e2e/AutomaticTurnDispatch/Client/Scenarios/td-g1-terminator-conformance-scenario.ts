// TD-G1: Cross-language source와 target도 같은 순서를 만든다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdG1 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdG1();
