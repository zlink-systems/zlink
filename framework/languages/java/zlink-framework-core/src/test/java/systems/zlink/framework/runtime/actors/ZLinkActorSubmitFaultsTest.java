package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;

final class ZLinkActorSubmitFaultsTest {
    @Test
    void submitRetryPolicyMatchesActorRelayFailures() {
        assertTrue(ZLinkActorSubmitFaults.retryableSubmitResult(SubmitResult.NOT_CONNECTED));
        assertTrue(ZLinkActorSubmitFaults.retryableSubmitResult(SubmitResult.BACKPRESSURED));
        assertTrue(ZLinkActorSubmitFaults.retryableSubmitResult(SubmitResult.NOT_FOUND));
        assertFalse(ZLinkActorSubmitFaults.retryableSubmitResult(SubmitResult.TERMINATED));
    }

    @Test
    void alreadyBoundAcceptsConflictBusyAndNativeBusyErrno() {
        assertTrue(ZLinkActorSubmitFaults.alreadyBound(request(RequestResult.CONFLICT)));
        assertTrue(ZLinkActorSubmitFaults.alreadyBound(request(RequestResult.BUSY)));
        assertTrue(ZLinkActorSubmitFaults.alreadyBound(new ZlinkRequestException(
            RequestResult.INTERNAL_ERROR,
            16)));
        assertFalse(ZLinkActorSubmitFaults.alreadyBound(request(RequestResult.NOT_FOUND)));
    }

    @Test
    void sessionActorAndBoundSessionBindRetryPoliciesStaySeparate() {
        Throwable timeout = request(RequestResult.TIMED_OUT);
        Throwable busy = request(RequestResult.BUSY);
        Throwable configNotFound = new ZlinkConfigException(ConfigResult.NOT_FOUND);

        assertTrue(ZLinkActorSubmitFaults.retryableSessionActorBindFailure(timeout));
        assertFalse(ZLinkActorSubmitFaults.retryableBoundSessionBindFailure(timeout));

        assertFalse(ZLinkActorSubmitFaults.retryableSessionActorBindFailure(busy));
        assertTrue(ZLinkActorSubmitFaults.retryableBoundSessionBindFailure(busy));

        assertFalse(ZLinkActorSubmitFaults.retryableSessionActorBindFailure(configNotFound));
        assertTrue(ZLinkActorSubmitFaults.retryableBoundSessionBindFailure(configNotFound));
    }

    @Test
    void requestNotFoundSearchesExceptionCauseChain() {
        Throwable wrapped = new RuntimeException(request(RequestResult.NOT_FOUND));

        assertTrue(ZLinkActorSubmitFaults.requestNotFound(wrapped));
        assertFalse(ZLinkActorSubmitFaults.requestNotFound(request(RequestResult.TERMINATED)));
    }

    private static ZlinkRequestException request(RequestResult result) {
        return new ZlinkRequestException(result);
    }
}
