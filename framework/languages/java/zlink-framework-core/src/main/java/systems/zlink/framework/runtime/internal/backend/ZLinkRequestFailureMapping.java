package systems.zlink.framework.runtime.internal.backend;

import static systems.zlink.framework.errors.ZLinkFrameworkErrorKind.*;
import static systems.zlink.framework.runtime.protocol.ServiceWireConstants.*;

import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

/** Maps Framework wire failure codes using error model section 2.1. */
public final class ZLinkRequestFailureMapping {
    private record Row(ZLinkFrameworkErrorKind kind, int terminal, int code, boolean receive) {}

    private static Row row(ZLinkFrameworkErrorKind kind, RequestResult terminal, long code) {
        return new Row(kind, terminal.value(), (int) code, true);
    }

    // Each kind's representative precedes its other codes.
    private static final Row[] ROWS = {
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_REQUEST_TARGET_NOT_FOUND),
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_ACTOR_ROUTE_NOT_FOUND),
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_SPOT_ROUTE_NOT_FOUND),
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_HANDLER_NOT_FOUND),
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_ROUTE_HANDLER_NOT_FOUND),
        row(NOT_FOUND, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_ACTOR_DISPATCH_HANDLER_NOT_FOUND),
        row(ALREADY_EXISTS, RequestResult.CONFLICT, FRAMEWORK_ERROR_ACTOR_ALREADY_EXISTS),
        row(TYPE_MISMATCH, RequestResult.CONFLICT, FRAMEWORK_ERROR_SPOT_TYPE_MISMATCH),
        row(TYPE_MISMATCH, RequestResult.CONFLICT, FRAMEWORK_ERROR_ACTOR_TYPE_MISMATCH),
        row(REJECTED, RequestResult.REJECTED, FRAMEWORK_ERROR_REQUEST_REJECTED),
        row(REJECTED, RequestResult.REJECTED, FRAMEWORK_ERROR_ACTOR_CREATE_REJECTED),
        row(UNAVAILABLE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED),
        row(UNAVAILABLE, RequestResult.REJECTED, FRAMEWORK_ERROR_WORKER_QUEUE_FULL),
        row(UNAVAILABLE, RequestResult.CONFLICT, FRAMEWORK_ERROR_ACTOR_LOCATION_STALE),
        row(UNAVAILABLE, RequestResult.CONFLICT, FRAMEWORK_ERROR_SPOT_MOVING),
        row(DEADLINE_EXCEEDED, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_WORKER_TIMED_OUT),
        row(SHUTTING_DOWN, RequestResult.TERMINATED, FRAMEWORK_ERROR_NONE),
        row(PROTOCOL_ERROR, RequestResult.PROTOCOL_ERROR, FRAMEWORK_ERROR_REQUEST_PROTOCOL_ERROR),
        row(PROTOCOL_ERROR, RequestResult.PROTOCOL_ERROR, FRAMEWORK_ERROR_PAYLOAD_DECODE_FAILED),
        row(INVALID_OPERATION, RequestResult.INVALID_STATE, FRAMEWORK_ERROR_NONE),
        row(INVALID_OPERATION, RequestResult.NOT_FOUND, FRAMEWORK_ERROR_ACTOR_SESSION_NOT_BOUND),
        row(INVALID_OPERATION, RequestResult.CONFLICT, FRAMEWORK_ERROR_SPOT_GENERATION_STALE),
        row(DATA_LOST, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_RELOCATION_DATA_LOST),
        row(INTERNAL_FAILURE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_REQUEST_FAILED),
        row(INTERNAL_FAILURE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_WORKER_FAILED),
        row(INTERNAL_FAILURE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_ACTOR_CREATE_FAILED),
        row(INTERNAL_FAILURE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_SPOT_CREATE_FAILED),
        new Row(
                NOT_CONFIGURED,
                RequestResult.INTERNAL_ERROR.value(),
                (int) FRAMEWORK_ERROR_REQUEST_FAILED,
                false),
    };

    private ZLinkRequestFailureMapping() {}

    private static final class ReceivedFailure
            extends systems.zlink.framework.errors.ZLinkFrameworkException {
        private final int failureCode;

        ReceivedFailure(
                ZLinkFrameworkErrorKind kind,
                String message,
                int failureCode,
                java.util.Map<String, String> metadata,
                Throwable cause) {
            super(kind, message, cause, metadata);
            this.failureCode = failureCode;
        }
    }

    public static systems.zlink.framework.errors.ZLinkFrameworkException receivedFailure(
            ZLinkFrameworkErrorKind kind,
            String message,
            int code,
            java.util.Map<String, String> metadata) {
        return receivedFailure(kind, message, code, metadata, null);
    }

    public static systems.zlink.framework.errors.ZLinkFrameworkException receivedFailure(
            ZLinkFrameworkErrorKind kind,
            String message,
            int code,
            java.util.Map<String, String> metadata,
            Throwable cause) {
        return new ReceivedFailure(kind, message, code, metadata, cause);
    }

    public static int causeCode(Throwable failure) {
        for (Throwable current = failure; current != null; current = current.getCause()) {
            if (current instanceof ReceivedFailure received
                    && received.failureCode != FRAMEWORK_ERROR_NONE) return received.failureCode;
            if (current.getCause() == current) break;
        }
        return (int) FRAMEWORK_ERROR_NONE;
    }

    public static int[] outgoing(ZLinkFrameworkErrorKind kind) {
        return outgoing(kind, (int) FRAMEWORK_ERROR_NONE);
    }

    public static int[] outgoing(ZLinkFrameworkErrorKind kind, int causeCode) {
        Row row = outgoingRow(kind, causeCode);
        return new int[] {row.terminal(), row.code()};
    }

    public static int outgoingCode(ZLinkFrameworkErrorKind kind, int causeCode) {
        return outgoingRow(kind, causeCode).code();
    }

    private static Row outgoingRow(ZLinkFrameworkErrorKind kind, int causeCode) {
        Row representative = null;
        for (Row row : ROWS) {
            if (row.kind() == kind) {
                if (causeCode == FRAMEWORK_ERROR_NONE) return row;
                if (representative == null) representative = row;
                if (causeCode != FRAMEWORK_ERROR_NONE && row.code() == causeCode) return row;
            }
        }
        return representative != null ? representative : outgoingRow(INTERNAL_FAILURE, 0);
    }

    /** Code absence is resolved by the caller's terminal result. */
    public static ZLinkFrameworkErrorKind incoming(int code) {
        if (code == FRAMEWORK_ERROR_NONE) return null;
        for (Row row : ROWS) {
            if (row.receive() && row.code() == code) return row.kind();
        }
        return null;
    }
}
