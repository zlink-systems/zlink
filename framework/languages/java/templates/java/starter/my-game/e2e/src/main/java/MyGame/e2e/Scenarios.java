package MyGame.e2e;

import MyGame.*;

import systems.zlink.stream.connector.*;

import java.net.URI;

public final class Scenarios {
    private Scenarios() {}

    // Add application scenarios to Scenario and zlink.structure.json together.
    static void run(Scenario scenario, Nodes nodes) throws Exception {
        switch (scenario) {
            case CONNECT -> connect(nodes);
        }
    }

    private static void connect(Nodes nodes) throws Exception {
        var connector =
                ZLinkStreamConnectorFactory.create(
                        ZLinkStreamConnectorOptions.createDefault(URI.create(nodes.endpoint)));
        try (AutoCloseable closing =
                () -> connector.close().submit().toCompletableFuture().join()) {
            connector.connect().submit().toCompletableFuture().join();
        }
        nodes.drain(nodes.sessionId);
    }
}
