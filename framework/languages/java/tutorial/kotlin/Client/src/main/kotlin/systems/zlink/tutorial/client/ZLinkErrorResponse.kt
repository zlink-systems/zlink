package systems.zlink.tutorial.client

import org.springframework.http.HttpStatus
import org.springframework.http.ResponseEntity
import org.springframework.web.bind.annotation.ExceptionHandler
import org.springframework.web.bind.annotation.RestControllerAdvice
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind
import systems.zlink.framework.errors.ZLinkFrameworkException

// --8<-- [start:error-mapping]
// A framework call fails by throwing ZLinkFrameworkException. Left alone, it
// reaches Spring Boot's default handler and every failure looks like a 500 --
// the caller cannot tell "no node is available right now" from "this server has
// a bug". This advice turns the error kind into the status code that says what
// actually happened.
@RestControllerAdvice
class ZLinkErrorResponse {

    // One handler standing in front of every controller in this application, so
    // no endpoint body carries a try/catch of its own.
    @ExceptionHandler(ZLinkFrameworkException::class)
    fun onFrameworkError(error: ZLinkFrameworkException): ResponseEntity<Map<String, String>> =
        ResponseEntity.status(statusFor(error.kind()))
            .body(mapOf("error" to nameFor(error.kind()), "message" to (error.message ?: "")))

    // The same table the framework's own HTTP host uses.
    private fun statusFor(kind: ZLinkFrameworkErrorKind): HttpStatus =
        when (kind) {
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkFrameworkErrorKind.TYPE_MISMATCH,
            ZLinkFrameworkErrorKind.INVALID_OPERATION -> HttpStatus.BAD_REQUEST
            ZLinkFrameworkErrorKind.NOT_FOUND -> HttpStatus.NOT_FOUND
            ZLinkFrameworkErrorKind.ALREADY_EXISTS -> HttpStatus.CONFLICT
            ZLinkFrameworkErrorKind.REJECTED -> HttpStatus.FORBIDDEN
            // No node can take the call now. The caller may retry.
            ZLinkFrameworkErrorKind.NOT_CONFIGURED,
            ZLinkFrameworkErrorKind.UNAVAILABLE,
            ZLinkFrameworkErrorKind.SHUTTING_DOWN -> HttpStatus.SERVICE_UNAVAILABLE
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED -> HttpStatus.GATEWAY_TIMEOUT
            else -> HttpStatus.INTERNAL_SERVER_ERROR
        }

    private fun nameFor(kind: ZLinkFrameworkErrorKind): String =
        when (kind) {
            ZLinkFrameworkErrorKind.NOT_FOUND -> "not_found"
            ZLinkFrameworkErrorKind.ALREADY_EXISTS -> "already_exists"
            ZLinkFrameworkErrorKind.TYPE_MISMATCH -> "type_mismatch"
            ZLinkFrameworkErrorKind.NOT_CONFIGURED -> "not_configured"
            ZLinkFrameworkErrorKind.REJECTED -> "rejected"
            ZLinkFrameworkErrorKind.UNAVAILABLE -> "unavailable"
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED -> "deadline_exceeded"
            ZLinkFrameworkErrorKind.SHUTTING_DOWN -> "shutting_down"
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR -> "protocol_error"
            ZLinkFrameworkErrorKind.INVALID_OPERATION -> "invalid_operation"
            ZLinkFrameworkErrorKind.DATA_LOST -> "data_lost"
            else -> "internal_failure"
        }
}
// --8<-- [end:error-mapping]
