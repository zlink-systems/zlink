package MyGame.e2e;

import MyGame.*;

import systems.zlink.stream.connector.*;

import java.net.URI;

public final class Scenarios {
    private Scenarios() {}

    // Add application scenarios to Scenario and zlink.structure.json together.
    static void run(Scenario scenario, Nodes nodes, boolean attach) throws Exception {
        switch (scenario) {
            case CONNECT -> {
                connect(nodes);
                if (attach) System.out.println("Skipping drain: requires an isolated server.");
                else nodes.drain(nodes.sessionId);
            }
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
    }
}
