package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

final class ZLinkActorSpotJoinCallTest {
    @Test
    void deadlineSaturationDoesNotTreatNegativeNanoTimeAsOverflow() {
        assertEquals(
            Long.MIN_VALUE + 50L,
            ZLinkActorSpotJoinCall.saturatedDeadline(Long.MIN_VALUE, 50L));
        assertEquals(
            Long.MAX_VALUE,
            ZLinkActorSpotJoinCall.saturatedDeadline(
                Long.MAX_VALUE - 1L,
                2L));
    }

    @Test
    void notConnectedRoutedJoinSubmitRejectCompletesUnavailable() {
        assertEquals(
            ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkActorSpotJoinCall.completionFailureKind(
                new CompletionException(new ZlinkRequestException(
                    RequestResult.NOT_CONNECTED,
                    109))));
        assertEquals(
            ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkActorSpotJoinCall.completionFailureKind(
                new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 109)));
        assertEquals(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            ZLinkActorSpotJoinCall.completionFailureKind(
                new IllegalStateException("unclassified join failure")));
    }

}
