package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.streams.ZLinkSession;

import java.util.Base64;
import java.util.function.Consumer;

final class ZLinkAdmissionQueueSettingsTest {
    private static final String KEY = key(32);
    private static final String PREVIOUS_KEY = key(64);

    @Test
    void disabledQueueIgnoresOtherQueueSettings() {
        var options = new DefaultZLinkFrameworkOptions();
        node(options, "one").admissionQueueKey("invalid").admissionGlobalLimit(0);
        node(options, "two").admissionQueuePreviousKey("invalid");
        assertDoesNotThrow(options::validate);
    }

    @Test
    void enabledQueueAcceptsMatchingSettingsAndOptionalValues() {
        var options = new DefaultZLinkFrameworkOptions();
        for (String name : new String[] {"one", "two"}) {
            enabled(options, name)
                    .admissionQueuePreviousKey(PREVIOUS_KEY)
                    .admissionGlobalLimit(Integer.MAX_VALUE);
        }
        assertDoesNotThrow(options::validate);

        var withoutOptional = new DefaultZLinkFrameworkOptions();
        enabled(withoutOptional, "one");
        assertDoesNotThrow(withoutOptional::validate);
    }

    @Test
    void enabledQueueRequiresAuthenticationPacketAndValidKeys() {
        invalid(builder -> builder.authenticationPacketName(null));
        invalid(builder -> builder.admissionQueueKey(null));
        invalid(builder -> builder.admissionQueueKey(key(31)));
        invalid(builder -> builder.admissionQueueKey(key(65)));
        invalid(builder -> builder.admissionQueueKey(KEY + "="));
        invalid(builder -> builder.admissionQueueKey(KEY.substring(0, KEY.length() - 1) + "B"));
        invalid(builder -> builder.admissionQueuePreviousKey(key(31)));
        invalid(builder -> builder.admissionQueuePreviousKey(key(65)));

        String secret = "invalid-secret-contains+";
        var failure = invalid(builder -> builder.admissionQueueKey(secret));
        assertFalse(failure.toString().contains(secret));
    }

    @Test
    void enabledQueueRequiresPositiveGlobalLimit() {
        invalid(builder -> builder.admissionGlobalLimit(0));
        invalid(builder -> builder.admissionGlobalLimit(-1));
        var options = new DefaultZLinkFrameworkOptions();
        enabled(options, "one").admissionGlobalLimit(1);
        assertDoesNotThrow(options::validate);
    }

    @Test
    void enabledQueueSettingsMustMatchAcrossRegistrations() {
        conflict(builder -> builder.admissionQueueEnabled(false));
        conflict(builder -> builder.admissionQueueKey(PREVIOUS_KEY));
        conflict(builder -> builder.admissionQueuePreviousKey(PREVIOUS_KEY));
        conflict(builder -> builder.admissionGlobalLimit(1));
        conflict(builder -> builder.admissionQueueMeshName("game"));
    }

    @Test
    void enabledQueueRequiresKnownMeshAndValidSpotId() {
        var unknownMesh = invalid(builder -> builder.admissionQueueMeshName("missing"));
        assertEquals("AdmissionQueueMeshName is not configured.", unknownMesh.getMessage());

        var options = new DefaultZLinkFrameworkOptions();
        options.addRouteMesh("가".repeat(80)).listen(0).setRoutingId(RoutingId.from("game"));
        enabled(options, "one");
        var failure = assertThrows(ZLinkConfigurationException.class, options::validate);
        assertEquals("Invalid admission queue SpotId.", failure.getMessage());
    }

    private static ZLinkConfigurationException invalid(Consumer<ZLinkStreamNodeBuilder> change) {
        var options = new DefaultZLinkFrameworkOptions();
        change.accept(enabled(options, "one"));
        return assertThrows(ZLinkConfigurationException.class, options::validate);
    }

    private static void conflict(Consumer<ZLinkStreamNodeBuilder> change) {
        var options = new DefaultZLinkFrameworkOptions();
        enabled(options, "one");
        change.accept(enabled(options, "two"));
        var error = assertThrows(ZLinkConfigurationException.class, options::validate);
        assertEquals(
                "STREAM registrations on a node must use the same authentication admission"
                        + " settings.",
                error.getMessage());
    }

    private static ZLinkStreamNodeBuilder enabled(
            DefaultZLinkFrameworkOptions options, String name) {
        if (options.registration().meshNodes().isEmpty()) {
            options.addRouteMesh("game").listen(0);
        }
        return node(options, name).admissionQueueEnabled(true).admissionQueueKey(KEY);
    }

    private static ZLinkStreamNodeBuilder node(DefaultZLinkFrameworkOptions options, String name) {
        return options.addStreamNode(name)
                .bind(0)
                .registerSession(name.equals("one") ? FirstSession.class : SecondSession.class)
                .authenticationPacketName("Login");
    }

    private static String key(int length) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(new byte[length]);
    }

    private abstract static class FirstSession implements ZLinkSession {}

    private abstract static class SecondSession implements ZLinkSession {}
}
