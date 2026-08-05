package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;

final class ZLinkActorSubmitFaults {
    private ZLinkActorSubmitFaults() {
    }

    static boolean retryableSubmitResult(SubmitResult result) {
        return result == SubmitResult.NOT_CONNECTED
            || result == SubmitResult.BACKPRESSURED
            || result == SubmitResult.NOT_FOUND;
    }

    static boolean alreadyBound(Throwable error) {
        ZlinkRequestException request = findRequestException(error);
        return request != null
            && (request.getResult() == RequestResult.CONFLICT
                || request.getResult() == RequestResult.BUSY
                || request.getNativeErrno() == 16);
    }

    static boolean retryableSessionActorBindFailure(Throwable error) {
        ZlinkRequestException request = findRequestException(error);
        if (request != null
            && (request.getResult() == RequestResult.NOT_CONNECTED
                || request.getResult() == RequestResult.NOT_FOUND
                || request.getResult() == RequestResult.TIMED_OUT)) {
            return true;
        }
        ZlinkSubmitException submit = findSubmitException(error);
        return submit != null && retryableSubmitResult(submit.getResult());
    }

    static boolean retryableBoundSessionBindFailure(Throwable error) {
        ZlinkRequestException request = findRequestException(error);
        if (request != null
            && (request.getResult() == RequestResult.NOT_FOUND
                || request.getResult() == RequestResult.NOT_CONNECTED
                || request.getResult() == RequestResult.BUSY
                || request.getNativeErrno() == 11
                || request.getNativeErrno() == 16)) {
            return true;
        }
        ZlinkSubmitException submit = findSubmitException(error);
        if (submit != null && retryableSubmitResult(submit.getResult())) {
            return true;
        }
        ZlinkConfigException config = findConfigException(error);
        return config != null && config.getResult() == ConfigResult.NOT_FOUND;
    }

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

    private static ZlinkConfigException findConfigException(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkConfigException config) {
                return config;
            }
            current = current.getCause();
        }
        return null;
    }

    private static ZlinkSubmitException findSubmitException(Throwable error) {
        Throwable current = error;
        while (current != null) {
            if (current instanceof ZlinkSubmitException submit) {
                return submit;
            }
            current = current.getCause();
        }
        return null;
    }
}
