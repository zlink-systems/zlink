package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;

final class ZLinkActorSubmitFaults {
    private ZLinkActorSubmitFaults() {}

    static boolean requestNotFound(Throwable error) {
        ZlinkRequestException request = findRequestException(error);
        return request != null && request.getResult() == RequestResult.NOT_FOUND;
    }

    private static ZlinkRequestException findRequestException(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkRequestException request) {
                return request;
            }
            current = current.getCause();
        }
        return null;
    }
}
