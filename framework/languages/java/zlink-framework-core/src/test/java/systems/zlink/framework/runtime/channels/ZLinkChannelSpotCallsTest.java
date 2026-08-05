package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.SpotHandles;

final class ZLinkChannelSpotCallsTest {
    @Test
    void staleRemoteHandleFailsWithTypedRouteNotFound() {
        CompletionException failure = org.junit.jupiter.api.Assertions.assertThrows(
            CompletionException.class,
            () -> SpotCallAddresses.resolve(
                    ignored -> CompletableFuture.completedFuture(Optional.empty()),
                    "stale-spot")
                .toCompletableFuture()
                .join());

        ZLinkFrameworkException typed = assertInstanceOf(
            ZLinkFrameworkException.class,
            failure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, typed.kind());
    }
}
