package systems.zlink.e2e.storefailure.client.scenarios;

import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfA1BaselineScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        //  단계마다 한 줄을 남긴다. 통과 시점에 아무 출력이 없으면 "무출력"과
        //  "무진행"을 구분할 수 없어 hang 조사에 매번 계측을 다시 심게 된다.
        System.out.println("[SF-A1] waitForLivePeerRows");
        context.waitForLivePeerRows();
        System.out.println("[SF-A1] waitForHealthyStatus");
        context.waitForHealthyStatus();
        System.out.println("[SF-A1] requestUntilAnyProvider");
        var result = context.requestUntilAnyProvider();
        System.out.println("[SF-A1] done");
        return result;
    }
}
