package systems.zlink.framework.runtime.channels;

import org.junit.jupiter.api.Assertions;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.spots.SpotHandles;

final class ZLinkChannelSpotCallsTest {
    @Test
    void staleRemoteHandleFailsWithTypedRouteNotFound() {
        CompletionException failure = Assertions.assertThrows(
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
        //  The local resolve failure is framework-generated, so it stays a
        //  stale-route control signal under the narrowed marker check.
        Assertions.assertTrue(SpotCallAddresses.isStaleRoute(typed));
    }

    //  D5: stale-route control requires kind NOT_FOUND plus the
    //  framework-origin marker; a preserved application NotFound kind or a
    //  framework error of another kind is not a stale route.
    @Test
    void staleRouteRequiresFrameworkOriginMarker() {
        Assertions.assertTrue(SpotCallAddresses.isStaleRoute(
            ZLinkFrameworkErrorOrigin.framework(
                ZLinkFrameworkErrorKind.NOT_FOUND, "stale route")));
        Assertions.assertFalse(SpotCallAddresses.isStaleRoute(
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "application entity not found")));
        Assertions.assertFalse(SpotCallAddresses.isStaleRoute(
            ZLinkFrameworkErrorOrigin.framework(
                ZLinkFrameworkErrorKind.REJECTED, "sealed admission")));
        Assertions.assertFalse(SpotCallAddresses.isStaleRoute(
            new IllegalStateException("unrelated")));
    }
}
