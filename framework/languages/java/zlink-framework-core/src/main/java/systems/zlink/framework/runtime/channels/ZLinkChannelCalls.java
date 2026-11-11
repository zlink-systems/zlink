package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkFanoutPublishCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.service.ZLinkClassicFanoutLiveness;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.spots.ZLinkSpotRequestCall;
import systems.zlink.framework.spots.ZLinkSpotSendCall;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;

public final class ZLinkChannelCalls implements ZLinkClient, ZLinkFanoutClient, ZLinkRouteClient {
    private final java.util.function.Supplier<ZLinkChannelRuntime> access;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkCodecRegistration codecs;
    private final Duration defaultRequestTimeout;
    private final Map<String, ZLinkChannelEnvelope.PreparedNames> channelEnvelopeNames;

    public ZLinkChannelCalls(
            ZLinkFrameworkRegistration registration,
            ZLinkMessageSerializer serializer,
            java.util.function.Supplier<ZLinkChannelRuntime> access) {
        this.access = access;
        this.serializer = serializer;
        this.codecs = registration.codecs();
        this.defaultRequestTimeout = registration.defaultRequestTimeout();
        Map<String, ZLinkChannelEnvelope.PreparedNames> preparedChannels =
                new java.util.HashMap<>();
        for (var mesh : registration.meshNodes()) {
            preparedChannels.putIfAbsent(mesh.meshName(), mesh.envelopeNames());
            for (String name : mesh.channelNames()) {
                preparedChannels.putIfAbsent(name, mesh.channelEnvelopeNames(name));
            }
        }
        for (ChannelRegistration channel : registration.channels()) {
            preparedChannels.put(channel.name(), channel.envelopeNames());
        }
        this.channelEnvelopeNames = Map.copyOf(preparedChannels);
    }

    @Override
    public ZLinkSendCall sendToChannel(String channelName, Object message) {
        rejectAfterRelocationReady("Channel send");
        String contentType = codecs.contentTypeFor(ZLinkPayloadEncoding.declaredType(message));
        String packetName = ZLinkPayloadEncoding.resolvePacketName(message);
        ZLinkFrameworkMultipartView payload =
                ZLinkPayloadEncoding.encodeMessage(serializer, message, contentType);
        return new ChannelSendCall(
                access, channelName, null, payload, packetName, contentType, null);
    }

    private ZLinkPayloadEncoding.EncodedPayload encodePayload(Object message) {
        Class<?> payloadType = ZLinkPayloadEncoding.declaredType(message);
        var selected = codecs.sendSelectionFor(payloadType);
        return ZLinkPayloadEncoding.encode(
                serializer, message, selected.contentType(), selected.names());
    }

    @Override
    public ZLinkRequestCall requestToChannel(String channelName, Object message) {
        rejectAfterRelocationReady("Channel request");
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        return new ChannelRequestCall(
                access,
                channelName,
                null,
                defaultRequestTimeout,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                null,
                encoded.contentType(),
                channelEnvelopeNames.get(channelName),
                encoded.names(),
                null);
    }

    @Override
    public ZLinkFanoutPublishCall publish(String channelName, Object message) {
        rejectAfterRelocationReady("Channel publish");
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        requireApplicationFanoutTopic(encoded.packetName());
        return new PublishCall(
                access,
                null,
                encoded.packetName(),
                encoded.payload().getFirst(),
                Optional.of(encoded.packetName()),
                encoded.contentType(),
                channelName);
    }

    @Override
    public ZLinkFanoutPublishCall publish(String channelName, String topic, Object message) {
        rejectAfterRelocationReady("Channel publish");
        requireApplicationFanoutTopic(topic);
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        return new PublishCall(
                access,
                null,
                topic,
                encoded.payload().getFirst(),
                Optional.of(encoded.packetName()),
                encoded.contentType(),
                channelName);
    }

    static void requireApplicationFanoutTopic(String topic) {
        if (topic == null) {
            throw new ZLinkConfigurationException("fanout topic is required");
        }
        requireWellFormedUnicodeTopic(topic);
        if (ZLinkClassicFanoutLiveness.startsWithReservedTopic(
                topic.getBytes(StandardCharsets.UTF_8))) {
            throw new ZLinkConfigurationException(
                    "fanout topic is reserved for transport liveness");
        }
    }

    private static void requireWellFormedUnicodeTopic(String topic) {
        for (int index = 0; index < topic.length(); index++) {
            char value = topic.charAt(index);
            if (Character.isHighSurrogate(value)) {
                if (index + 1 >= topic.length()
                        || !Character.isLowSurrogate(topic.charAt(index + 1))) {
                    throw new ZLinkConfigurationException("fanout topic must be valid Unicode");
                }
                index++;
            } else if (Character.isLowSurrogate(value)) {
                throw new ZLinkConfigurationException("fanout topic must be valid Unicode");
            }
        }
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                .rejectAfterRelocationReady(operation);
    }

    @Override
    public ZLinkSendCall sendToNode(String meshName, RoutingId target, Object message) {
        String contentType = codecs.contentTypeFor(ZLinkPayloadEncoding.declaredType(message));
        String packetName = ZLinkPayloadEncoding.resolvePacketName(message);
        ZLinkFrameworkMultipartView payload =
                ZLinkPayloadEncoding.encodeMessage(serializer, message, contentType);
        return new RouteSendCall(
                access, meshName, null, target, payload, packetName, contentType, null);
    }

    @Override
    public ZLinkSpotSendCall sendToSpot(String spotId, Object message) {
        Objects.requireNonNull(spotId, "spotId");
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        return new RouteSpotSendCall(
                access,
                null,
                null,
                null,
                spotId,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                encoded.contentType(),
                false,
                null,
                null,
                ZLinkApplicationMetadata.empty());
    }

    @Override
    public ZLinkRequestCall requestToNode(String meshName, RoutingId target, Object message) {
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        return new RouteRequestCall(
                access,
                meshName,
                null,
                defaultRequestTimeout,
                target,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                null,
                encoded.contentType(),
                channelEnvelopeNames.get(meshName),
                encoded.names(),
                null);
    }

    @Override
    public ZLinkSpotRequestCall requestToSpot(String spotId, Object message) {
        Objects.requireNonNull(spotId, "spotId");
        ZLinkPayloadEncoding.EncodedPayload encoded = encodePayload(message);
        return new RouteSpotRequestCall(
                access,
                null,
                null,
                null,
                null,
                spotId,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                defaultRequestTimeout,
                encoded.contentType(),
                null,
                encoded.names(),
                false,
                null,
                null,
                ZLinkApplicationMetadata.empty());
    }
}
