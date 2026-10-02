package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;

final class ZLinkActorSubmitFaultsTest {
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
