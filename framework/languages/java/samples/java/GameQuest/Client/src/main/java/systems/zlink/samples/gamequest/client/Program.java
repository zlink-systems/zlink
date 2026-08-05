package systems.zlink.samples.gamequest.client;

import java.net.URI;
import java.time.Duration;
import systems.zlink.samples.gamequest.server.configuration.SampleNames;
import systems.zlink.samples.gamequest.server.configuration.SampleTimings;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        GameQuestClientOptions options = GameQuestClientOptions.load(args);
        ZLinkStreamConnector apiA = createClient(options.apiAStreamEndpoint());
        ZLinkStreamConnector apiB = createClient(options.apiBStreamEndpoint());
        try {
            GameQuestClientScenario scenario = new GameQuestClientScenario(options, Program::createClient);
            if ("full".equals(options.scenario())) {
                scenario.run(apiA, apiB);
            } else if ("rehydrate".equals(options.scenario())) {
                scenario.verifyRehydrated(apiA);
            } else {
                throw new IllegalArgumentException("Unknown sample.scenario: " + options.scenario());
            }
        } finally {
            apiA.close().submit().toCompletableFuture().join();
            apiB.close().submit().toCompletableFuture().join();
        }
        System.out.println(SampleNames.CompletedMarker);
    }

    private static ZLinkStreamConnector createClient(String endpoint) {
        return ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            SampleTimings.RequestTimeout,
            SampleTimings.RequestTimeout,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Integer.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            null,
            null,
            null,
            null));
    }
}
