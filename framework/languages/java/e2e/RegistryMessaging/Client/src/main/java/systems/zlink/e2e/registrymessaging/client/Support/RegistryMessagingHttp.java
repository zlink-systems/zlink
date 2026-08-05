package systems.zlink.e2e.registrymessaging.client.Support;

import java.time.Duration;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RegistryMessagingHttp implements AutoCloseable {
    private final ZLinkHttpClient providerA;
    private final ZLinkHttpClient providerB;
    private final ZLinkHttpClient workflow;
    private final ZLinkHttpClient discoveryConsumer;
    private final ZLinkHttpClient directConsumer;
    private final ZLinkHttpClient singleConsumer;
    private final ZLinkHttpClient backpressureConsumer;

    public RegistryMessagingHttp(ClientOptions options) {
        providerA = client(options.providerAHttpUrl());
        providerB = client(options.providerBHttpUrl());
        workflow = client(options.workflowHttpUrl());
        discoveryConsumer = client(options.discoveryConsumerHttpUrl());
        directConsumer = client(options.directConsumerHttpUrl());
        singleConsumer = client(options.singleConsumerHttpUrl());
        backpressureConsumer = client(options.backpressureConsumerHttpUrl());
    }

    public ZLinkHttpClient providerA() {
        return providerA;
    }

    public ZLinkHttpClient providerB() {
        return providerB;
    }

    public ZLinkHttpClient workflow() {
        return workflow;
    }

    public ZLinkHttpClient discoveryConsumer() {
        return discoveryConsumer;
    }

    public ZLinkHttpClient directConsumer() {
        return directConsumer;
    }

    public ZLinkHttpClient singleConsumer() {
        return singleConsumer;
    }

    public ZLinkHttpClient backpressureConsumer() {
        return backpressureConsumer;
    }

    @Override
    public void close() {
        providerA.close();
        providerB.close();
        workflow.close();
        discoveryConsumer.close();
        directConsumer.close();
        singleConsumer.close();
        backpressureConsumer.close();
    }

    private static ZLinkHttpClient client(String endpoint) {
        return ZLinkHttpClient.create(endpoint)
            .timeout(Duration.ofMinutes(5))
            .build();
    }
}
