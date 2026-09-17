package systems.zlink.tutorial.client;

import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

// --8<-- [start:error-mapping]
// A framework call fails by throwing ZLinkFrameworkException. Left alone, it
// reaches Spring's default error handling and every failure looks like a 500 —
// the caller cannot tell "no node is available right now" from "this server has
// a bug". This advice turns the error kind into the status code that says what
// actually happened.
//
// One advice covers every endpoint, so no call site needs a try/catch. It also
// covers the endpoints that return CompletionStage: Spring unwraps the stage's
// failure before handing it to the handler below.
@RestControllerAdvice
public class ZLinkErrorResponse {

    // The same table the framework's own HTTP host uses.
    private static HttpStatus statusFor(ZLinkFrameworkErrorKind kind) {
        return switch (kind) {
            case PROTOCOL_ERROR, TYPE_MISMATCH, INVALID_OPERATION -> HttpStatus.BAD_REQUEST;
            case NOT_FOUND -> HttpStatus.NOT_FOUND;
            case ALREADY_EXISTS -> HttpStatus.CONFLICT;
            case REJECTED -> HttpStatus.FORBIDDEN;
            // No node can take the call now. The caller may retry.
            case NOT_CONFIGURED, UNAVAILABLE, SHUTTING_DOWN -> HttpStatus.SERVICE_UNAVAILABLE;
            case DEADLINE_EXCEEDED -> HttpStatus.GATEWAY_TIMEOUT;
            default -> HttpStatus.INTERNAL_SERVER_ERROR;
        };
    }

    private static String nameFor(ZLinkFrameworkErrorKind kind) {
        return switch (kind) {
            case NOT_FOUND -> "not_found";
            case ALREADY_EXISTS -> "already_exists";
            case TYPE_MISMATCH -> "type_mismatch";
            case NOT_CONFIGURED -> "not_configured";
            case REJECTED -> "rejected";
            case UNAVAILABLE -> "unavailable";
            case DEADLINE_EXCEEDED -> "deadline_exceeded";
            case SHUTTING_DOWN -> "shutting_down";
            case PROTOCOL_ERROR -> "protocol_error";
            case INVALID_OPERATION -> "invalid_operation";
            case DATA_LOST -> "data_lost";
            default -> "internal_failure";
        };
    }

    @ExceptionHandler(ZLinkFrameworkException.class)
    ResponseEntity<Body> frameworkError(ZLinkFrameworkException error) {
        return ResponseEntity.status(statusFor(error.kind()))
            .contentType(MediaType.APPLICATION_JSON)
            .body(new Body(nameFor(error.kind()), error.getMessage()));
    }

    // Same body as the other languages: the kind's name, then the message the
    // framework raised.
    record Body(String error, String message) {
    }
}
// --8<-- [end:error-mapping]
