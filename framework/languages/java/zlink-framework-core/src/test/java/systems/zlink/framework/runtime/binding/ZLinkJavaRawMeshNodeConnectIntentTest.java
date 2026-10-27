package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;

import java.util.UUID;

final class ZLinkJavaRawMeshNodeConnectIntentTest {
    @RepeatedTest(20)
    void repeatedConfigurationRetainsOneIntent() {
        String endpoint = "inproc://connect-intent-" + UUID.randomUUID();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setBind(endpoint + "-source");
            source.start();
            long endpointIntent = source.connectPeer(endpoint);
            assertEquals(endpointIntent, source.connectPeer(endpoint));
            source.removePeerConnection(endpointIntent);
            long descriptorIntent =
                    source.connectPeer(endpoint, RoutingId.from("peer"), 17, "default");
            assertNotEquals(endpointIntent, descriptorIntent);
            assertEquals(
                    descriptorIntent,
                    source.connectPeer(endpoint, RoutingId.from("peer"), 17, "default"));
            assertEquals(
                    descriptorIntent,
                    source.replacePeerConnection(endpoint, RoutingId.from("peer"), 17, "default"));
            source.removePeerConnection(descriptorIntent);
            assertNotEquals(
                    descriptorIntent,
                    source.connectPeer(endpoint, RoutingId.from("replacement"), 19, "default"));
        }
    }
}
