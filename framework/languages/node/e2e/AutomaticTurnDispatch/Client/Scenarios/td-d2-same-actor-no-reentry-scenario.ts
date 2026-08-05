// TD-D2: 같은 Actor의 다음 record는 Yield continuation 뒤 실행한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdD2 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdD2();
