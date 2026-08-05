// TD-E2: PerActor와 SpotWide에서 같은 deferred Join 의미를 사용한다 시나리오를 검증한다.
import type { ExecutionTurnScenarioSuite } from '../Support/execution-turn-scenario-suite';
export const runTdE2 = (suite: ExecutionTurnScenarioSuite): Promise<void> => suite.tdE2();
