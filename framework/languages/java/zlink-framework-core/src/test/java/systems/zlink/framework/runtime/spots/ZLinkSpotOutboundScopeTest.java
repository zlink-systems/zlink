package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkConfigurationException;

final class ZLinkSpotOutboundScopeTest {
    @Test
    void ambientOutboundRejectsCallsOutsideSpotCallback() {
        ZLinkSpotOutboundScope scope = new ZLinkSpotOutboundScope();

        assertThrows(
            ZLinkConfigurationException.class,
            () -> scope.ambient().publish("channel", "topic", "message"));
    }
}
