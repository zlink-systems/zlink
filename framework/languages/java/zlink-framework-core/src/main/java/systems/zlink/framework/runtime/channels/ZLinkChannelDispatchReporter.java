package systems.zlink.framework.runtime.channels;

import java.util.List;
import java.util.concurrent.CompletionException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

final class ZLinkChannelDispatchReporter {
    private final ZLinkDispatchErrorReporter reporter;
    ZLinkChannelDispatchReporter(ZLinkDispatchErrorReporter reporter) {
        this.reporter = reporter;
    }

    void replyError(
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        long requestSeq,
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorReason reason,
        String packetName,
        String channelName,
        String sourceRid,
        Throwable error) {
        Throwable cause = unwrap(error);
        List<Message> reply = ZLinkFrameworkErrorReply.create(
            frameworkErrorKind(error),
            errorText(reason, packetName, cause));
        replyRawAndClose(router, routingId, requestSeq, reply);
        report(
            surface,
            kind,
            reason,
            ZLinkDispatchErrorAction.REPLY_ERROR,
            packetName,
            channelName,
            sourceRid,
            cause);
    }

    void report(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        String packetName,
        String channelName,
        String sourceRid,
        Throwable error) {
        report(surface, kind, reason, action, packetName, channelName, null, sourceRid, error);
    }

    void report(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind kind,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        String packetName,
        String channelName,
        String topic,
        String sourceRid,
        Throwable error) {
        Throwable cause = unwrap(error);
        reporter.report(new ZLinkDispatchFailure(
            surface,
            kind,
            reason,
            action,
            packetName == null || packetName.isBlank() ? null : packetName,
            channelName,
            topic,
            null,
            null,
            sourceRid,
            null,
            errorType(cause),
            errorMessage(cause)));
    }

    static ZLinkDispatchErrorReason reasonFrom(Throwable error) {
        return unwrap(error) instanceof PayloadDecodeDispatchException
            ? ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED
            : ZLinkDispatchErrorReason.HANDLER_EXCEPTION;
    }

    static ZLinkFrameworkErrorKind frameworkErrorKind(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZLinkFrameworkException frameworkError) {
            return frameworkError.kind();
        }
        if (cause instanceof PayloadDecodeDispatchException decode
            && decode.getCause() instanceof ZLinkFrameworkException frameworkError) {
            return frameworkError.kind();
        }
        return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
    }

    static void replyAndClose(
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        long requestSeq,
        Message reply) {
        try {
            router.reply(routingId, requestSeq, List.of(reply));
        } finally {
            reply.close();
        }
    }

    static void replyRawAndClose(
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        long requestSeq,
        List<Message> reply) {
        try {
            router.reply(routingId, requestSeq, reply);
        } finally {
            reply.forEach(Message::close);
        }
    }

    private static Throwable unwrap(Throwable error) {
        return error instanceof CompletionException && error.getCause() != null
            ? error.getCause()
            : error;
    }

    private static String errorType(Throwable error) {
        return error == null ? null : error.getClass().getSimpleName();
    }

    private static String errorMessage(Throwable error) {
        return error == null ? null : error.getMessage();
    }

    private static String errorText(
        ZLinkDispatchErrorReason reason,
        String packetName,
        Throwable error) {
        if (error != null && error.getMessage() != null) {
            return error.getMessage();
        }
        return reason + " for packet '" + packetName + "'";
    }
}
