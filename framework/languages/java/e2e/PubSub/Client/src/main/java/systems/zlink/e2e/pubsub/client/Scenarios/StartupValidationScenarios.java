package systems.zlink.e2e.pubsub.client.Scenarios;

import systems.zlink.e2e.pubsub.client.Support.ScenarioContext;

public final class StartupValidationScenarios {
    private StartupValidationScenarios() { }

    // PS-E2A: automatic subscriber의 Store 누락을 startup에서 거부한다.
    public static void runE2A(ScenarioContext context) {
        context.processes().expectAutomaticSubscriberWithoutStoreFailure();
        passed("PS-E2A");
    }

    // PS-E2B: automatic과 manual subscriber mode 혼합을 startup에서 거부한다.
    public static void runE2B(ScenarioContext context) {
        context.processes().expectMixedSubscriberModeFailure();
        passed("PS-E2B");
    }

    // PS-E2C: automatic publisher identity 누락과 중복을 startup에서 거부한다.
    public static void runE2C(ScenarioContext context) {
        context.processes().expectPublisherIdentityFailures();
        passed("PS-E2C");
    }

    private static void passed(String selector) {
        System.out.println("scenario " + selector + " passed");
    }
}
