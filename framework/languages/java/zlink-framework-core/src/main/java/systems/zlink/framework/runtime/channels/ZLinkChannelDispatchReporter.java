package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.function.Consumer;

final class ZLinkChannelDispatchReporter {
    private final ZLinkDispatchErrorReporter reporter;
    private final ZLinkCodecRegistration codecs;

    ZLinkChannelDispatchReporter(
            ZLinkDispatchErrorReporter reporter, ZLinkCodecRegistration codecs) {
        this.reporter = reporter;
        this.codecs = codecs;
    }

    void replyRegisteredPayloadAndClose(
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            ZLinkChannelEnvelope.Header header,
            Message reply,
            ZLinkChannelEnvelope.PreparedNames channelNames,
            ZLinkChannelEnvelope.PreparedNames names) {
        replyPartsAndClose(
                router, received, replyParts(header, reply, channelNames, names, codecs));
    }

    static void onAdmissionRejected(
            CompletableFuture<Void> admission, Runnable release, Consumer<Throwable> rejected) {
        admission.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) {
                        release.run();
                        if (rejected != null) rejected.accept(failure);
                    }
                });
    }

    void replyShutdownOnAdmissionRejected(
            CompletableFuture<Void> admission,
            Message payload,
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            ZLinkDispatchErrorSurface surface,
            String packetName,
            String channelName,
            String sourceRid,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader) {
        onAdmissionRejected(
                admission,
                payload::close,
                failure ->
                        replyError(
                                router,
                                received,
                                surface,
                                ZLinkDispatchMessageKind.REQUEST,
                                ZLinkDispatchErrorReason.SHUTDOWN,
                                packetName,
                                channelName,
                                sourceRid,
                                requestHeader,
                                new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                        "channel runtime is closing",
                                        failure)));
    }

    void replyError(
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind kind,
            ZLinkDispatchErrorReason reason,
            String packetName,
            String channelName,
            String sourceRid,
            Throwable error) {
        replyError(
                router,
                received,
                surface,
                kind,
                reason,
                packetName,
                channelName,
                sourceRid,
                null,
                error);
    }

    void replyError(
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind kind,
            ZLinkDispatchErrorReason reason,
            String packetName,
            String channelName,
            String sourceRid,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Throwable error) {
        if (received.hasDirectReplyPath()) {
            replyError(
                    received,
                    surface,
                    kind,
                    reason,
                    packetName,
                    channelName,
                    sourceRid,
                    requestHeader,
                    error);
            return;
        }
        replyError(
                router,
                received.routingId().orElseThrow(),
                received.requestSeq().orElseThrow(),
                surface,
                kind,
                reason,
                packetName,
                channelName,
                sourceRid,
                requestHeader,
                error);
    }

    void replyError(
            ZLinkBackendReceived received,
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind kind,
            ZLinkDispatchErrorReason reason,
            String packetName,
            String channelName,
            String sourceRid,
            Throwable error) {
        replyError(
                received, surface, kind, reason, packetName, channelName, sourceRid, null, error);
    }

    void replyError(
            ZLinkBackendReceived received,
            ZLinkDispatchErrorSurface surface,
            ZLinkDispatchMessageKind kind,
            ZLinkDispatchErrorReason reason,
            String packetName,
            String channelName,
            String sourceRid,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Throwable error) {
        Throwable cause = unwrap(error);
        List<Message> reply =
                ZLinkFrameworkErrorReply.create(
                        requestHeader,
                        frameworkErrorKind(error),
                        errorText(reason, packetName, cause),
                        java.util.Map.of());
        replyRawAndClose(received, reply);
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
                router,
                routingId,
                requestSeq,
                surface,
                kind,
                reason,
                packetName,
                channelName,
                sourceRid,
                null,
                error);
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
        List<Message> reply =
                ZLinkFrameworkErrorReply.create(
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
        reporter.report(
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
                error);
    }

    static ZLinkDispatchErrorReason reasonFrom(Throwable error) {
        return unwrap(error) instanceof PayloadDecodeDispatchException
                ? ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED
                : systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter.reasonFrom(
                        frameworkErrorKind(error));
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
            ZLinkBackendRouterSocket router, ZLinkBackendReceived received, Message reply) {
        replyPayloadAndClose(router, received, null, reply);
    }

    static void replyAndClose(ZLinkBackendReceived received, Message reply) {
        try {
            received.reply(List.of(reply));
        } finally {
            reply.close();
        }
    }

    static void replyAndClose(
            ZLinkBackendRouterSocket router, RoutingId routingId, long requestSeq, Message reply) {
        try {
            router.reply(routingId, requestSeq, List.of(reply));
        } finally {
            reply.close();
        }
    }

    /**
     * Writes a successful request reply. An envelope request gets a kind-2 envelope reply echoing
     * the request identifiers (shared cross-language wire); a legacy raw request keeps the raw
     * single-part reply.
     */
    static void replyPayloadAndClose(
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            ZLinkFrameworkMultipartView reply) {
        replyPartsAndClose(router, received, replyParts(requestHeader, reply, null, null, null));
    }

    private static void replyPartsAndClose(
            ZLinkBackendRouterSocket router, ZLinkBackendReceived received, List<Message> parts) {
        try {
            if (received.hasDirectReplyPath()) received.reply(parts);
            else
                router.reply(
                        received.routingId().orElseThrow(),
                        received.requestSeq().orElseThrow(),
                        parts);
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }

    static void replyPayloadAndClose(
            ZLinkBackendReceived received,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            ZLinkFrameworkMultipartView reply) {
        List<Message> parts = replyParts(requestHeader, reply, null, null, null);
        try {
            received.reply(parts);
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }

    static void replyPayloadAndClose(
            ZLinkBackendRouterSocket router,
            RoutingId routingId,
            long requestSeq,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            ZLinkFrameworkMultipartView reply) {
        List<Message> parts = replyParts(requestHeader, reply, null, null, null);
        try {
            router.reply(routingId, requestSeq, parts);
        } finally {
            ZLinkFrameworkMultipartView.closeAll(parts);
        }
    }

    static List<Message> replyParts(
            ZLinkChannelEnvelope.Header requestHeader,
            ZLinkFrameworkMultipartView reply,
            ZLinkChannelEnvelope.PreparedNames channelNames,
            ZLinkChannelEnvelope.PreparedNames names,
            ZLinkCodecRegistration codecs) {
        if (requestHeader == null) return reply;
        try {
            return ZLinkChannelEnvelope.encode(
                    ZLinkChannelEnvelope.reply(requestHeader),
                    reply,
                    channelNames,
                    names,
                    codecs == null
                            ? null
                            : codecs.envelopeContentNamesFor(requestHeader.contentType()));
        } catch (RuntimeException | Error failure) {
            reply.close();
            throw failure;
        }
    }

    static List<Message> replyParts(
            ZLinkChannelEnvelope.Header requestHeader,
            Message reply,
            ZLinkChannelEnvelope.PreparedNames channelNames,
            ZLinkChannelEnvelope.PreparedNames names,
            ZLinkCodecRegistration codecs) {
        if (requestHeader == null) return List.of(reply);
        try {
            return ZLinkChannelEnvelope.encode(
                    ZLinkChannelEnvelope.reply(requestHeader),
                    reply,
                    channelNames,
                    names,
                    codecs == null
                            ? null
                            : codecs.envelopeContentNamesFor(requestHeader.contentType()));
        } catch (RuntimeException | Error failure) {
            reply.close();
            throw failure;
        }
    }

    static void replyPayloadAndClose(
            ZLinkBackendRouterSocket router,
            ZLinkBackendReceived received,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Message reply) {
        replyPartsAndClose(router, received, replyParts(requestHeader, reply, null, null, null));
    }

    static void replyPayloadAndClose(
            ZLinkBackendReceived received,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Message reply) {
        replyPayloadAndClose(received, requestHeader, ZLinkFrameworkMultipartView.owned(reply));
    }

    static void replyPayloadAndClose(
            ZLinkBackendRouterSocket router,
            RoutingId routingId,
            long requestSeq,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Message reply) {
        replyPayloadAndClose(
                router,
                routingId,
                requestSeq,
                requestHeader,
                ZLinkFrameworkMultipartView.owned(reply));
    }

    static void replyRawAndClose(ZLinkBackendReceived received, List<Message> reply) {
        try {
            received.reply(reply);
        } finally {
            reply.forEach(Message::close);
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

    private static String errorText(
            ZLinkDispatchErrorReason reason, String packetName, Throwable error) {
        if (error != null && error.getMessage() != null) {
            return error.getMessage();
        }
        return reason + " for packet '" + packetName + "'";
    }
}
