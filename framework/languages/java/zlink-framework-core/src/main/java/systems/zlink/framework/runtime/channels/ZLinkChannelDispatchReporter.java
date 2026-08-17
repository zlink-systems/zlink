package systems.zlink.framework.runtime.channels;

import java.util.List;
import java.util.concurrent.CompletionException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchFailure;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
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
        replyError(
            router, routingId, requestSeq, surface, kind, reason,
            packetName, channelName, sourceRid, null, error);
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
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
        Throwable error) {
        Throwable cause = unwrap(error);
        List<Message> reply = ZLinkFrameworkErrorReply.create(
            requestHeader,
            frameworkErrorKind(error),
            errorText(reason, packetName, cause),
            java.util.Map.of());
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
        if (cause instanceof PayloadDecodeDispatchException decode) {
            //  Spec 32-framework-error-model:40 — an unprocessable payload is a
            //  ProtocolError even when it arrives without a nested framework kind.
            return decode.getCause() instanceof ZLinkFrameworkException frameworkError
                ? frameworkError.kind()
                : ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
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

    /**
     * Writes a successful request reply. An envelope request gets a kind-2
     * envelope reply echoing the request identifiers (shared cross-language
     * wire); a legacy raw request keeps the raw single-part reply.
     */
    static void replyPayloadAndClose(
        ZLinkBackendRouterSocket router,
        RoutingId routingId,
        long requestSeq,
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
        Message reply) {
        if (requestHeader == null) {
            replyAndClose(router, routingId, requestSeq, reply);
            return;
        }
        Message replyHeader = systems.zlink.framework.runtime.messaging
            .ZLinkChannelEnvelope.encodeHeader(
                systems.zlink.framework.runtime.messaging
                    .ZLinkChannelEnvelope.reply(requestHeader));
        try {
            router.reply(routingId, requestSeq, List.of(replyHeader, reply));
        } finally {
            replyHeader.close();
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
