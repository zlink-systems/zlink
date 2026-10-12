package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertNotNull;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotHandlerPreparationTest {
    @Test
    void spotInterfaceMethodIsSelectedDuringRegistration() {
        var registration =
                new SpotPacketHandlerRegistration(
                        Handler.class,
                        null,
                        Object.class,
                        String.class,
                        Void.class,
                        "message",
                        false,
                        java.util.List.of());
        assertNotNull(registration.invocation().method());
    }

    public static final class Handler {
        public CompletionStage<Void> handle(
                Object spot, String message, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
