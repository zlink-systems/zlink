package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;

import java.lang.reflect.InvocationTargetException;
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
        ErrorDetails errorDetails = errorDetails(error);
        reportedCount.incrementAndGet();
        tracePoint.trace(
                ZLinkMessageFlowEvent.dispatchError(
                        surface,
                        messageKind,
                        packetName,
                        channelName,
                        topic,
                        correlationId instanceof Long value
                                ? Long.toUnsignedString(value.longValue())
                                : correlationId == null ? null : correlationId.toString(),
                        sourceRid == null ? null : sourceRid.toString(),
                        spotId == null ? null : spotId.toString(),
                        actorId,
                        reason,
                        action,
                        errorDetails.type(),
                        errorDetails.message()));
    }

    public long reportedCount() {
        return reportedCount.get();
    }

    static ErrorDetails errorDetails(Throwable error) {
        if (error == null) {
            return new ErrorDetails(null, null);
        }
        Throwable current = error;
        while ((current instanceof CompletionException
                        || current instanceof InvocationTargetException)
                && current.getCause() != null) {
            current = current.getCause();
        }
        if (current instanceof ZLinkConfigurationException
                && current.getCause() != null
                && current.getMessage() != null
                && current.getMessage().startsWith("failed to invoke ")) {
            current = current.getCause();
        }
        String message = current.getMessage();
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
