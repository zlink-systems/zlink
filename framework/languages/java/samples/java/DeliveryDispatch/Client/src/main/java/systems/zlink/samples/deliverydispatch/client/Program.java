package systems.zlink.samples.deliverydispatch.client;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

import java.net.URI;
import java.time.Duration;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleNames;
import systems.zlink.samples.deliverydispatch.server.configuration.SampleTimings;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ClientOptions options = ClientOptions.load(args);
        ZLinkStreamConnector customer = createClient(options.customerStreamEndpoint());
        ZLinkStreamConnector courierA = createClient(options.courierStreamEndpoint());
        ZLinkStreamConnector courierB = createClient(options.courierStreamEndpoint());
        ScheduledExecutorService processLifetime =
            Executors.newSingleThreadScheduledExecutor();
        processLifetime.schedule(() -> { }, 5, TimeUnit.MINUTES);
        new DeliveryDispatchClientScenario(options.dispatchHttpEndpoint()).run(customer, courierA, courierB)
            .toCompletableFuture()
            .orTimeout(2, TimeUnit.MINUTES)
            .whenComplete((ignored, error) -> CompletableFuture.allOf(
                    customer.close().submit().toCompletableFuture(),
                    courierA.close().submit().toCompletableFuture(),
                    courierB.close().submit().toCompletableFuture())
                .whenComplete((closed, closeError) -> {
                    if (error != null) {
                        error.printStackTrace(System.err);
                        processLifetime.shutdownNow();
                        System.exit(1);
                    }
                    if (closeError != null) {
                        closeError.printStackTrace(System.err);
                        processLifetime.shutdownNow();
                        System.exit(1);
                    }
                    System.out.println(SampleNames.CompletedMarker);
                    processLifetime.shutdownNow();
                }));
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
