package systems.zlink.e2e.registrationcodec.client.Scenarios;

import java.util.List;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcA4DiLifecycleScenario {
    private RcA4DiLifecycleScenario() {
    }

    public static void run(ScenarioContext context) {
        List<Contracts.DiLifecycleRes> replies = List.of(
            context.server().post("/registration/di-filter-order").submit(Contracts.DiLifecycleRes[].class).toCompletableFuture().join().body());
        ScenarioAssert.ensure(replies.stream().map(Contracts.DiLifecycleRes::scopedId).distinct().count() == 3,
            "RC-A4 scoped dependency was not recreated per request: " + replies);
        ScenarioAssert.ensure(replies.stream().map(Contracts.DiLifecycleRes::singletonId).distinct().count() == 1,
            "RC-A4 singleton dependency changed between requests: " + replies);
        ScenarioAssert.ensure(replies.get(2).disposedCount() == 3,
            "RC-A4 dispose count mismatch: " + replies);
        ScenarioAssert.waitForEvidenceValueSuffix(context.evidence(), "DI", "DiLifecycle", ":di-2");
        System.out.println("scenario RC-A4 passed");
    }
}
