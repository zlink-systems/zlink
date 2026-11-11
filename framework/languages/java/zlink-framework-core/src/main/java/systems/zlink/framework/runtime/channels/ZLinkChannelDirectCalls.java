package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.channels.ZLinkFanoutPublishCall;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

final class PublishCall implements ZLinkFanoutPublishCall, Runnable {
    private final AtomicBoolean submitGate;
    private final java.util.function.Supplier<ZLinkChannelRuntime> host;
    private ZLinkChannelCallRuntime runtime;
    private ZLinkBackendPublisherSocket publisher;
    private final String topic;
    private String publisherChannel;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;

    PublishCall(
            ZLinkChannelCallRuntime runtime,
            ZLinkBackendPublisherSocket publisher,
            String topic,
            Message payload,
            Optional<String> packetName) {
        this(
                runtime,
                publisher,
                topic,
                payload,
                packetName,
                ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE);
    }

    PublishCall(
            ZLinkChannelCallRuntime runtime,
            ZLinkBackendPublisherSocket publisher,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType) {
        this(
                runtime,
                publisher,
                topic,
                payload,
                packetName,
                contentType,
                new AtomicBoolean(),
                null);
    }

    private PublishCall(
            ZLinkChannelCallRuntime runtime,
            ZLinkBackendPublisherSocket publisher,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType,
            AtomicBoolean submitGate,
            java.util.function.Supplier<ZLinkChannelRuntime> host) {
        this.submitGate = submitGate;
        this.host = host;
        this.runtime = runtime;
        this.publisher = publisher;
        this.topic = topic;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
    }

    PublishCall(
            ZLinkChannelCallRuntime runtime,
            ZLinkBackendPublisherSocket publisher,
            String topic,
            Message payload) {
        this(runtime, publisher, topic, payload, Optional.empty());
    }

    PublishCall(
            java.util.function.Supplier<ZLinkChannelRuntime> host,
            ZLinkBackendPublisherSocket publisher,
            String topic,
            Message payload,
            Optional<String> packetName,
            String contentType,
            String publisherChannel) {
        this(null, publisher, topic, payload, packetName, contentType, new AtomicBoolean(), host);
        this.publisherChannel = publisherChannel;
    }

    @Override
    public void run() {
        if (host == null) return;
        var live = host.get();
        runtime = live.callRuntimeForCalls();
        publisher = live.requirePublisher(publisherChannel);
    }

    public ZLinkFanoutPublishCall packetName(String packetName) {
        PublishCall copy =
                new PublishCall(
                        runtime,
                        publisher,
                        topic,
                        payload,
                        Optional.of(packetName),
                        contentType,
                        submitGate,
                        host);
        copy.publisherChannel = publisherChannel;
        return copy;
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate, this);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope = runtime.enterApplicationFlow()) {
            List<Message> publishParts =
                    ZLinkChannelCallRuntime.parts(packetName, payload, contentType);
            try {
                publisher.publish(topic, publishParts, SendFlags.NONE);
                return ZLinkOneWayCalls.oneWayStatus(ZLinkOneWayCalls.SUBMITTED);
            } catch (ZlinkSubmitException failure) {
                if (failure.getResult()
                        == systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED) {
                    return ZLinkOneWayCalls.oneWayStatus(ZLinkOneWayCalls.TIMED_OUT);
                }
                return ZLinkOneWayCalls.adaptOneWay(CompletableFuture.failedFuture(failure));
            } finally {
                publishParts.forEach(Message::close);
            }
        }
    }
}
