package systems.zlink.framework.runtime.internal.transport;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ZLinkListenerIdentityTest {
    @Test
    void omittedAdvertiseHostMapsWildcardBindToSameFamilyLoopback() {
        assertEquals(
            "tcp://127.0.0.1:43120",
            ZLinkListenerIdentity.advertisedEndpoint(
                "tcp://0.0.0.0:43120", null));
        assertEquals(
            "tcp://[::1]:43121",
            ZLinkListenerIdentity.advertisedEndpoint(
                "tcp://[::]:43121", null));
    }

    @Test
    void explicitWildcardAdvertiseHostIsRejected() {
        for (String wildcard : new String[] {
                "0.0.0.0", "::", "[::]", "0:0:0:0:0:0:0:0"}) {
            assertThrows(
                ZLinkConfigurationException.class,
                () -> ZLinkListenerIdentity.advertisedEndpoint(
                    "tcp://127.0.0.1:43120", wildcard));
        }
    }
}
