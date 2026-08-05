// TD-F3: Session relay로 시작한 Actor handler에서도 같은 의미를 사용한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdF3 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdF3();
