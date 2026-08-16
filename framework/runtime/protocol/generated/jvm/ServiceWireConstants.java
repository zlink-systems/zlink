// Generated from service-wire-v1.schema.json. Do not edit.
package systems.zlink.framework.runtime.protocol;

public final class ServiceWireConstants {
    public static final int MAGIC_0 = 90;
    public static final int MAGIC_1 = 77;
    public static final int WIRE_MAJOR = 1;
    public static final String REQUIRED_CAPABILITY = "framework-service-v12";
    public static final String FRAMEWORK_MULTIPART_PACKET_NAME = "ZLinkFrameworkMultipart";
    public static final String FRAMEWORK_MULTIPART_CONTENT_TYPE = "application/x-zlink-multipart";
    public static final int COMMAND_HELLO = 1;
    public static final int COMMAND_ADMIT = 2;
    public static final int COMMAND_REJECT = 3;
    public static final int COMMAND_UPDATE = 4;
    public static final int COMMAND_LIVENESS_PROBE = 5;
    public static final int COMMAND_LIVENESS_ACK = 6;
    public static final int COMMAND_NODE_SEND = 16;
    public static final int COMMAND_NODE_REQUEST = 17;
    public static final int COMMAND_CHANNEL_SEND = 18;
    public static final int COMMAND_CHANNEL_REQUEST = 19;
    public static final int COMMAND_REPLY = 20;
    public static final int COMMAND_SPOT_SEND = 21;
    public static final int COMMAND_SPOT_REQUEST = 22;
    public static final int COMMAND_LOGICAL_MULTICAST = 23;
    public static final int COMMAND_ACTOR_SEND = 24;
    public static final int COMMAND_ACTOR_REQUEST = 25;
    public static final int COMMAND_ACTOR_LOOKUP = 26;
    public static final int COMMAND_ACTOR_DESTROY = 27;
    public static final int COMMAND_ACTOR_JOIN = 28;
    public static final int COMMAND_ACTOR_LEFT = 29;
    public static final int COMMAND_RELOCATION_READY = 30;
    public static final int COMMAND_RELOCATION_DATA = 31;
    public static final int COMMAND_REPLY_RELAY = 33;
    public static final int COMMAND_RELOCATION_CUTOVER = 34;
    public static final int COMMAND_BOUND_SESSION_SEND = 36;
    public static final int COMMAND_ACTOR_JOINED = 37;
    public static final int COMMAND_BOUND_SESSION_BIND = 38;
    public static final int COMMAND_INSTANCE_SPOT = 39;
    public static final int COMMAND_RELOCATION_PREPARE = 40;
    public static final int COMMAND_SESSION_RELOCATION_SEAL = 42;
    public static final int COMMAND_SESSION_RELOCATION_SEALED = 43;
    public static final int COMMAND_SESSION_RELOCATION_ROUTE = 44;
    public static final int COMMAND_REPLY_RELAY_ACK = 46;
    public static final int COMMAND_USER_SPOT_CREATE = 47;
    public static final int COMMAND_USER_SPOT_CLOSE = 48;
    public static final int COMMAND_ACTOR_CREATE = 49;
    public static final int COMMAND_MESSAGE_FOLLOW = 50;
    public static final int COMMAND_BOUND_SESSION_REPLACED = 51;
    public static final int FLAG_METADATA = 1;
    public static final int FLAG_BOUND_SESSION = 2;
    public static final int FLAG_SOURCE_SPOT_ID = 4;
    public static final int FLAG_EXTENSION = 8;
    public static final long FRAMEWORK_ERROR_NONE = 0L;
    public static final long FRAMEWORK_ERROR_ACTOR_ROUTE_NOT_FOUND = 1L;
    public static final long FRAMEWORK_ERROR_ACTOR_CREATE_FAILED = 2L;
    public static final long FRAMEWORK_ERROR_ACTOR_ALREADY_EXISTS = 3L;
    public static final long FRAMEWORK_ERROR_ACTOR_TYPE_MISMATCH = 4L;
    public static final long FRAMEWORK_ERROR_SPOT_CREATE_FAILED = 5L;
    public static final long FRAMEWORK_ERROR_SPOT_ROUTE_NOT_FOUND = 6L;
    public static final long FRAMEWORK_ERROR_SPOT_TYPE_MISMATCH = 7L;
    public static final long FRAMEWORK_ERROR_ACTOR_SESSION_NOT_BOUND = 8L;
    public static final long FRAMEWORK_ERROR_HANDLER_NOT_FOUND = 9L;
    public static final long FRAMEWORK_ERROR_ROUTE_HANDLER_NOT_FOUND = 10L;
    public static final long FRAMEWORK_ERROR_ACTOR_DISPATCH_HANDLER_NOT_FOUND = 11L;
    public static final long FRAMEWORK_ERROR_PAYLOAD_DECODE_FAILED = 12L;
    public static final long FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED = 13L;
    public static final long FRAMEWORK_ERROR_REQUEST_TARGET_NOT_FOUND = 14L;
    public static final long FRAMEWORK_ERROR_REQUEST_REJECTED = 15L;
    public static final long FRAMEWORK_ERROR_REQUEST_PROTOCOL_ERROR = 16L;
    public static final long FRAMEWORK_ERROR_REQUEST_FAILED = 17L;
    public static final long FRAMEWORK_ERROR_WORKER_QUEUE_FULL = 18L;
    public static final long FRAMEWORK_ERROR_WORKER_TIMED_OUT = 19L;
    public static final long FRAMEWORK_ERROR_WORKER_FAILED = 20L;
    public static final long FRAMEWORK_ERROR_ACTOR_LOCATION_STALE = 21L;
    public static final long FRAMEWORK_ERROR_ACTOR_CREATE_REJECTED = 22L;
    public static final long FRAMEWORK_ERROR_SPOT_GENERATION_STALE = 33L;
    public static final long FRAMEWORK_ERROR_SPOT_MOVING = 34L;
    public static final long FRAMEWORK_ERROR_RELOCATION_DATA_LOST = 35L;
    //  Schema terminal-failure-integrity: success is ok+none, boundary
    //  terminals carry none, typed failures must match their exact terminal,
    //  and an unknown failure code is a protocol error before dispatch.
    public static boolean validTerminalFailure(long terminal, long failureCode) {
        if (terminal == 0L) {
            return failureCode == 0L;
        }
        switch ((int) terminal) {
            case 101:
            case 103:
            case 108:
            case 109:
            case 110:
            case 111:
            case 112:
            case 113:
                return failureCode == 0L;
            default:
                break;
        }
        if (failureCode == 0L) {
            return false;
        }
        switch ((int) failureCode) {
            case 1: // actorRouteNotFound
            case 6: // spotRouteNotFound
            case 8: // actorSessionNotBound
            case 9: // handlerNotFound
            case 10: // routeHandlerNotFound
            case 11: // actorDispatchHandlerNotFound
            case 14: // requestTargetNotFound
                return terminal == 102L;
            case 2: // actorCreateFailed
            case 5: // spotCreateFailed
            case 13: // routeNotConnected
            case 17: // requestFailed
            case 19: // workerTimedOut
            case 20: // workerFailed
            case 35: // relocationDataLost
                return terminal == 105L;
            case 3: // actorAlreadyExists
            case 4: // actorTypeMismatch
            case 7: // spotTypeMismatch
            case 21: // actorLocationStale
            case 33: // spotGenerationStale
            case 34: // spotMoving
                return terminal == 107L;
            case 12: // payloadDecodeFailed
            case 16: // requestProtocolError
                return terminal == 104L;
            case 15: // requestRejected
            case 18: // workerQueueFull
            case 22: // actorCreateRejected
                return terminal == 106L;
            default:
                return false;
        }
    }
    private ServiceWireConstants() {}
}
