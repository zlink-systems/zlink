package systems.zlink.framework.runtime.internal.backend;

import static systems.zlink.framework.errors.ZLinkFrameworkErrorKind.*;
import static systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping.Context.*;
import static systems.zlink.framework.runtime.protocol.ServiceWireConstants.*;

import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

import java.util.Set;

/** Owns outgoing representatives, incoming aliases and existing typed-path aliases. */
public final class ZLinkRequestFailureMapping {
    public enum Context {
        GENERAL,
        ACTOR_JOIN,
        SUPERSEDED_ACTOR_JOIN,
        ACTOR_RELOCATION,
        SPOT_RELOCATION
    }

    private record Row(
            ZLinkFrameworkErrorKind kind,
            int terminal,
            int code,
            ZLinkFrameworkErrorKind relocationKind,
            Set<Context> outgoing) {}

    private static Row row(
            ZLinkFrameworkErrorKind kind,
            RequestResult terminal,
            long code,
            ZLinkFrameworkErrorKind relocationKind,
            Context... outgoing) {
        return new Row(kind, terminal.value(), (int) code, relocationKind, Set.of(outgoing));
    }

    // General representatives occur first; context aliases preserve existing typed paths.
    private static final Row[] ROWS = {
        row(
                NOT_FOUND,
                RequestResult.NOT_FOUND,
                FRAMEWORK_ERROR_REQUEST_TARGET_NOT_FOUND,
                NOT_FOUND,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                ALREADY_EXISTS,
                RequestResult.CONFLICT,
                FRAMEWORK_ERROR_ACTOR_ALREADY_EXISTS,
                ALREADY_EXISTS,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                TYPE_MISMATCH,
                RequestResult.CONFLICT,
                FRAMEWORK_ERROR_SPOT_TYPE_MISMATCH,
                TYPE_MISMATCH,
                GENERAL,
                SPOT_RELOCATION),
        row(
                REJECTED,
                RequestResult.REJECTED,
                FRAMEWORK_ERROR_REQUEST_REJECTED,
                REJECTED,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                UNAVAILABLE,
                RequestResult.INTERNAL_ERROR,
                FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED,
                UNAVAILABLE,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                DEADLINE_EXCEEDED,
                RequestResult.INTERNAL_ERROR,
                FRAMEWORK_ERROR_WORKER_TIMED_OUT,
                DEADLINE_EXCEEDED,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                SHUTTING_DOWN,
                RequestResult.TERMINATED,
                FRAMEWORK_ERROR_NONE,
                null,
                GENERAL,
                ACTOR_JOIN),
        row(
                PROTOCOL_ERROR,
                RequestResult.PROTOCOL_ERROR,
                FRAMEWORK_ERROR_REQUEST_PROTOCOL_ERROR,
                PROTOCOL_ERROR,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                INVALID_OPERATION,
                RequestResult.INVALID_STATE,
                FRAMEWORK_ERROR_NONE,
                null,
                GENERAL,
                ACTOR_JOIN),
        row(
                DATA_LOST,
                RequestResult.INTERNAL_ERROR,
                FRAMEWORK_ERROR_RELOCATION_DATA_LOST,
                DATA_LOST,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                INTERNAL_FAILURE,
                RequestResult.INTERNAL_ERROR,
                FRAMEWORK_ERROR_REQUEST_FAILED,
                INTERNAL_FAILURE,
                GENERAL,
                ACTOR_JOIN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                TYPE_MISMATCH,
                RequestResult.CONFLICT,
                FRAMEWORK_ERROR_ACTOR_TYPE_MISMATCH,
                TYPE_MISMATCH,
                ACTOR_JOIN,
                ACTOR_RELOCATION),
        row(
                INVALID_OPERATION,
                RequestResult.NOT_FOUND,
                FRAMEWORK_ERROR_ACTOR_SESSION_NOT_BOUND,
                null),
        row(
                NOT_FOUND,
                RequestResult.NOT_FOUND,
                FRAMEWORK_ERROR_HANDLER_NOT_FOUND,
                NOT_CONFIGURED,
                ACTOR_RELOCATION,
                SPOT_RELOCATION),
        row(
                PROTOCOL_ERROR,
                RequestResult.PROTOCOL_ERROR,
                FRAMEWORK_ERROR_PAYLOAD_DECODE_FAILED,
                null),
        row(UNAVAILABLE, RequestResult.REJECTED, FRAMEWORK_ERROR_WORKER_QUEUE_FULL, UNAVAILABLE),
        row(INTERNAL_FAILURE, RequestResult.INTERNAL_ERROR, FRAMEWORK_ERROR_WORKER_FAILED, null),
        row(
                UNAVAILABLE,
                RequestResult.CONFLICT,
                FRAMEWORK_ERROR_ACTOR_LOCATION_STALE,
                INVALID_OPERATION,
                SUPERSEDED_ACTOR_JOIN,
                ACTOR_RELOCATION),
        row(
                INVALID_OPERATION,
                RequestResult.CONFLICT,
                FRAMEWORK_ERROR_SPOT_GENERATION_STALE,
                INVALID_OPERATION,
                SPOT_RELOCATION),
        row(UNAVAILABLE, RequestResult.CONFLICT, FRAMEWORK_ERROR_SPOT_MOVING, null),
        row(
                SHUTTING_DOWN,
                RequestResult.INTERNAL_ERROR,
                FRAMEWORK_ERROR_REQUEST_FAILED,
                SHUTTING_DOWN,
                ACTOR_RELOCATION,
                SPOT_RELOCATION)
    };

    private ZLinkRequestFailureMapping() {}

    public static int[] outgoing(ZLinkFrameworkErrorKind kind, Context context) {
        Row row = outgoingRow(kind, context);
        return new int[] {row.terminal(), row.code()};
    }

    public static int outgoingCode(ZLinkFrameworkErrorKind kind, Context context) {
        return outgoingRow(kind, context).code();
    }

    private static Row outgoingRow(ZLinkFrameworkErrorKind kind, Context context) {
        boolean relocation = context == ACTOR_RELOCATION || context == SPOT_RELOCATION;
        for (Row row : ROWS) {
            ZLinkFrameworkErrorKind outgoingKind = relocation ? row.relocationKind() : row.kind();
            if (row.outgoing().contains(context)
                    && (context == SUPERSEDED_ACTOR_JOIN || outgoingKind == kind)) {
                return row;
            }
        }
        return outgoingRow(INTERNAL_FAILURE, GENERAL);
    }

    /** Fine-code absence is resolved by the caller's coarse terminal owner. */
    public static ZLinkFrameworkErrorKind incoming(int code, Context context) {
        if (code == FRAMEWORK_ERROR_NONE) {
            return null;
        }
        boolean relocation = context == ACTOR_RELOCATION || context == SPOT_RELOCATION;
        for (Row row : ROWS) {
            if (row.code() == code) {
                return relocation ? row.relocationKind() : row.kind();
            }
        }
        return null;
    }
}
