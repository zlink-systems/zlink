package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchFailure;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

import java.lang.reflect.InvocationTargetException;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Pattern;

public final class ZLinkDispatchErrorReporter {
    private static final int ERROR_MESSAGE_MAX_LENGTH = 512;
    private static final CredentialPattern[] CREDENTIAL_PATTERNS = {
        new CredentialPattern(
                Pattern.compile(
                        "Authorization\\s*:\\s*(?:(?:Bearer|Basic)\\s+)?[^\\s,;]+",
                        Pattern.CASE_INSENSITIVE),
                "Authorization: <redacted>"),
        new CredentialPattern(
                Pattern.compile("Bearer\\s+[^\\s,;]+", Pattern.CASE_INSENSITIVE),
                "Bearer <redacted>"),
        new CredentialPattern(
                Pattern.compile("password\\s*=\\s*[^\\s,;]+", Pattern.CASE_INSENSITIVE),
                "password=<redacted>"),
        new CredentialPattern(
                Pattern.compile("token\\s*=\\s*[^\\s,;]+", Pattern.CASE_INSENSITIVE),
                "token=<redacted>")
    };
    private final AtomicLong reportedCount = new AtomicLong();
    // Success-path tracer companion: every surface already receives a reporter, so
    // exposing the flow tracer here wires all dispatch sites without threading a new
    // parameter. Shares the same options (live mode), factory and executor.
    private final ZLinkMessageFlowTracer flow;

    public ZLinkDispatchErrorReporter(
            ZLinkDispatchOptionsRegistration options,
            ZLinkHandlerActivator handlerFactory,
            Executor executor) {
        this(options, handlerFactory, executor, null);
    }

    public ZLinkDispatchErrorReporter(
            ZLinkDispatchOptionsRegistration options,
            ZLinkHandlerActivator handlerFactory,
            Executor executor,
            ZLinkRuntimeEventDispatcher eventDispatcher) {
        this.flow = new ZLinkMessageFlowTracer(options, handlerFactory, executor, eventDispatcher);
    }

    public ZLinkMessageFlowTracer flow() {
        return flow;
    }

    public void report(
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind messageKind,
            ZLinkDispatchErrorReason reason,
            ZLinkDispatchErrorAction action,
            String packetName,
            String channelName,
            String topic,
            Object spotId,
            String actorId,
            Object sourceRid,
            Object correlationId,
            Throwable error) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.beginDispatchError();
        if (tracePoint == null) {
            return;
        }
        report(
                tracePoint,
                surface,
                messageKind,
                reason,
                action,
                packetName,
                channelName,
                topic,
                sourceRid == null ? null : sourceRid.toString(),
                correlationId instanceof Long value
                        ? Long.toUnsignedString(value.longValue())
                        : correlationId == null ? null : correlationId.toString(),
                spotId == null ? null : spotId.toString(),
                actorId,
                null,
                null,
                null,
                errorDetails(reason, error));
    }

    /** Whether a dispatch error record would be written at the live level. */
    public boolean errorsEnabled() {
        return flow.dispatchErrorsEnabled();
    }

    public void report(ZLinkDispatchFailure error) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.beginDispatchError();
        if (tracePoint == null) {
            return;
        }
        ErrorDetails details =
                error.failure() == null
                        ? new ErrorDetails(error.errorType(), error.errorMessage())
                        : errorDetails(error.reason(), error.failure());
        report(
                tracePoint,
                error.surface(),
                error.messageKind(),
                error.reason(),
                error.action(),
                error.packetName(),
                error.channelName(),
                error.topic(),
                error.sourceRid(),
                error.correlationId(),
                error.spotId(),
                error.actorId(),
                error.meshName(),
                error.targetRid(),
                error.streamSessionId(),
                details);
    }

    private void report(
            ZLinkMessageFlowTracer.TracePoint tracePoint,
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind messageKind,
            ZLinkDispatchErrorReason reason,
            ZLinkDispatchErrorAction action,
            String packetName,
            String channelName,
            String topic,
            String sourceRid,
            String correlationId,
            String spotId,
            String actorId,
            String meshName,
            String targetRid,
            String streamSessionId,
            ErrorDetails errorDetails) {
        reportedCount.incrementAndGet();
        ZLinkMessageFlowEvent event =
                failureEvent(
                        surface,
                        messageKind,
                        packetName,
                        channelName,
                        topic,
                        correlationId,
                        sourceRid,
                        spotId,
                        actorId,
                        meshName,
                        targetRid,
                        reason,
                        action,
                        errorDetails);
        tracePoint.trace(
                streamSessionId == null ? event : event.withStreamSessionId(streamSessionId));
    }

    static ZLinkMessageFlowEvent failureEvent(
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind messageKind,
            String packetName,
            String channelName,
            String topic,
            String correlationId,
            String sourceRid,
            String spotId,
            String actorId,
            String meshName,
            String targetRid,
            ZLinkDispatchErrorReason reason,
            ZLinkDispatchErrorAction action,
            ErrorDetails details) {
        return ZLinkMessageFlowEvent.dispatchError(
                surface,
                messageKind,
                packetName,
                channelName,
                topic,
                correlationId,
                sourceRid,
                spotId,
                actorId,
                meshName,
                targetRid,
                reason,
                action,
                details.type(),
                details.message());
    }

    public static ZLinkDispatchErrorReason reasonFrom(Throwable error, int failureCode) {
        Throwable submission =
                error instanceof systems.zlink.contracts.errors.ZlinkSubmitException
                        ? error
                        : error == null ? null : error.getCause();
        if (submission instanceof systems.zlink.contracts.errors.ZlinkSubmitException submit) {
            switch (submit.getResult()) {
                case NOT_CONNECTED, NOT_FOUND -> {
                    return ZLinkDispatchErrorReason.STALE_TARGET;
                }
                case BACKPRESSURED -> {
                    return ZLinkDispatchErrorReason.BACKPRESSURE;
                }
                default -> {}
            }
        }
        if (failureCode
                        == systems.zlink.framework.runtime.protocol.ServiceWireConstants
                                .FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED
                || failureCode
                        == systems.zlink.framework.runtime.protocol.ServiceWireConstants
                                .FRAMEWORK_ERROR_SPOT_MOVING) {
            return ZLinkDispatchErrorReason.STALE_TARGET;
        }
        return error instanceof systems.zlink.framework.errors.ZLinkFrameworkException framework
                ? reasonFrom(framework.kind())
                : ZLinkDispatchErrorReason.HANDLER_EXCEPTION;
    }

    public static ZLinkDispatchErrorReason reasonFrom(
            systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind) {
        return switch (kind) {
            case NOT_FOUND -> ZLinkDispatchErrorReason.HANDLER_MISSING;
            case PROTOCOL_ERROR -> ZLinkDispatchErrorReason.INVALID_FRAME;
            case SHUTTING_DOWN -> ZLinkDispatchErrorReason.SHUTDOWN;
            default -> ZLinkDispatchErrorReason.HANDLER_EXCEPTION;
        };
    }

    public long reportedCount() {
        return reportedCount.get();
    }

    static ErrorDetails errorDetails(Throwable error) {
        return errorDetails(null, error);
    }

    static ErrorDetails errorDetails(ZLinkDispatchErrorReason reason, Throwable error) {
        if (error == null) {
            return new ErrorDetails(null, null);
        }
        Throwable current = error;
        while ((current instanceof CompletionException
                        || current instanceof InvocationTargetException)
                && current.getCause() != null) {
            current = current.getCause();
        }
        Throwable diagnostic = current;
        if (reason == ZLinkDispatchErrorReason.ENCODE_ERROR && diagnostic.getCause() != null) {
            var visited = Collections.newSetFromMap(new IdentityHashMap<Throwable, Boolean>());
            visited.add(diagnostic);
            while (diagnostic.getCause() != null && visited.add(diagnostic.getCause())) {
                diagnostic = diagnostic.getCause();
            }
        }
        String message = diagnostic.getMessage();
        if (message == null) {
            message = "";
        }
        if (!message.isEmpty()) {
            int carriageReturn = message.indexOf('\r');
            int lineFeed = message.indexOf('\n');
            int lineEnd =
                    carriageReturn < 0
                            ? lineFeed
                            : lineFeed < 0 ? carriageReturn : Math.min(carriageReturn, lineFeed);
            if (lineEnd >= 0) {
                message = message.substring(0, lineEnd);
            }
            for (CredentialPattern credential : CREDENTIAL_PATTERNS) {
                message =
                        credential.pattern().matcher(message).replaceAll(credential.replacement());
            }
            if (message.length() > ERROR_MESSAGE_MAX_LENGTH) {
                message = message.substring(0, ERROR_MESSAGE_MAX_LENGTH);
            }
        }
        return new ErrorDetails(current.getClass().getSimpleName(), message);
    }

    record ErrorDetails(String type, String message) {}

    private record CredentialPattern(Pattern pattern, String replacement) {}
}
