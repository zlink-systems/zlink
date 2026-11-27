package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.spots.ZLinkRemoteSpotRequestFixture.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.UnaryOperator;

final class ZLinkSpotLookupDeadlineTest {
    @Test
    void requestDeadlineExpiresWhileReadySpotAuthorityReadIsPending() throws Exception {
        var hold = new AtomicBoolean();
        var readStarted = new CompletableFuture<Void>();
        var heldRead = new CompletableFuture<ZLinkAuthorityReadResult>();
        String authorityKey = ZLinkAuthorityKeyCodec.spot(SPOT);

        try (var fixture =
                new ZLinkRemoteSpotRequestFixture(
                        UnaryOperator.identity(),
                        store ->
                                intercept(
                                        store,
                                        "read",
                                        args -> {
                                            if (!hold.get() || !authorityKey.equals(args[0]))
                                                return null;
                                            readStarted.complete(null);
                                            return heldRead;
                                        }))) {
            hold.set(true);
            ZLinkFrameworkRuntimeTestAccess.discovery(fixture.requester).invalidateSpotRoute(SPOT);
            try {
                var request = fixture.request("held", Duration.ofSeconds(1));

                await(readStarted);
                var failure = assertThrows(ExecutionException.class, () -> await(request));
                assertEquals(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
                assertFalse(
                        heldRead.isDone(), "The request must finish with the authority read held");
                assertEquals(0, ZLinkRemoteSpotFirstRequestTest.CALLS.get());
            } finally {
                heldRead.cancel(false);
            }
        }
    }
}
