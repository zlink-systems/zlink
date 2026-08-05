package systems.zlink.samples.bingo.client;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import systems.zlink.samples.bingo.client.configuration.SampleTimings;
import systems.zlink.samples.bingo.client.configuration.SampleTopology;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.framework.codecs.protobuf.ZLinkProtobufCodec;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) throws Exception {
        SampleTopology topology = SampleTopology.load(args);
        ZLinkStreamConnector client1 = createClient(topology.sessionAStreamEndpoint());
        ZLinkStreamConnector client2 = createClient(topology.sessionBStreamEndpoint());
        ZLinkStreamConnector observer = createClient(topology.sessionBStreamEndpoint());
        try {
            new BingoClientScenario().run(client1, client2, observer);
        } finally {
            client1.close().submit().toCompletableFuture().join();
            client2.close().submit().toCompletableFuture().join();
            observer.close().submit().toCompletableFuture().join();
        }
        System.out.println("bingo=completed");
    }

    private static ZLinkStreamConnector createClient(String endpoint) {
        ZLinkStreamConnector client = ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            SampleTimings.RequestTimeout,
            2,
            SampleTimings.ConnectTimeout,
            64 * 1024,
            false,
            Duration.ofSeconds(1),
            SampleTimings.RequestTimeout.plusSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            ZLinkProtobufCodec.defaultCodec()));
        client.observeInbound(observation -> {
            System.out.println(
                "stream-inbound sample=Bingo kind=" + observation.kind()
                    + " name=" + observation.packetName()
                    + " bytes=" + observation.payloadLength());
            return CompletableFuture.completedFuture(null);
        });
        return client;
    }
}
