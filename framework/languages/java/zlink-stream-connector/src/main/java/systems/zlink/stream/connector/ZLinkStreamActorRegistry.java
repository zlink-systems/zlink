package systems.zlink.stream.connector;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.function.Consumer;

final class ZLinkStreamActorRegistry {
    static final String BOUND = "$zlink.actor.bound";
    static final String UNBOUND = "$zlink.actor.unbound";
    private static final int VERSION = 1;

    private final DefaultZLinkStreamConnector connector;
    private final ZLinkStreamConnectorConfiguration configuration;
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final Consumer<ZLinkStreamError> errorPublisher;
    private final Map<Integer, DefaultActor> bySlot = new LinkedHashMap<>();
    private final Map<String, DefaultActor> byId = new LinkedHashMap<>();
    private final List<ZLinkStreamActorHandler> boundHandlers = new CopyOnWriteArrayList<>();
    private final List<ZLinkStreamActorHandler> unboundHandlers = new CopyOnWriteArrayList<>();

    ZLinkStreamActorRegistry(
            DefaultZLinkStreamConnector connector,
            ZLinkStreamConnectorConfiguration configuration,
            ZLinkStreamDispatchQueue dispatchQueue,
            Consumer<ZLinkStreamError> errorPublisher) {
        this.connector = connector;
        this.configuration = configuration;
        this.dispatchQueue = dispatchQueue;
        this.errorPublisher = errorPublisher;
    }

    synchronized List<ZLinkStreamActor> snapshot() {
        return List.copyOf(bySlot.values());
    }

    synchronized Optional<ZLinkStreamActor> find(String actorId) {
        return Optional.ofNullable(byId.get(actorId));
    }

    synchronized String actorId(int slot) {
        DefaultActor actor = bySlot.get(slot);
        if (actor == null) {
            throw new IllegalArgumentException("packet uses an unregistered Actor slot");
        }
        return actor.actorId();
    }

    synchronized DefaultActor actor(int slot) {
        DefaultActor actor = bySlot.get(slot);
        if (actor == null) {
            throw new IllegalArgumentException("packet uses an unregistered Actor slot");
        }
        return actor;
    }

    synchronized int currentSlot(DefaultActor actor) {
        if (!actor.isBound() || bySlot.get(actor.slot()) != actor) {
            throw ZLinkStreamException.validationFailed("bound Actor handle is closed");
        }
        return actor.slot();
    }

    synchronized List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> handlers(
            int slot, String name) {
        DefaultActor actor = bySlot.get(slot);
        return actor == null ? List.of() : List.copyOf(actor.handlers(name));
    }

    void bound(byte[] payload) {
        ByteBuffer buffer = ByteBuffer.wrap(payload);
        require(buffer, 4, "Actor bound control");
        int version = Byte.toUnsignedInt(buffer.get());
        int slot = Short.toUnsignedInt(buffer.getShort());
        int idLength = Byte.toUnsignedInt(buffer.get());
        if (version != VERSION || slot == 0 || idLength == 0 || buffer.remaining() != idLength) {
            throw new IllegalArgumentException("Actor bound control payload is invalid");
        }
        byte[] id = new byte[idLength];
        buffer.get(id);
        String actorId = decodeActorId(id);
        DefaultActor actor;
        synchronized (this) {
            if (bySlot.containsKey(slot) || byId.containsKey(actorId)) {
                throw new IllegalArgumentException("Actor binding control is duplicated");
            }
            actor = new DefaultActor(connector, slot, actorId);
            bySlot.put(slot, actor);
            byId.put(actorId, actor);
        }
        publish(boundHandlers, actor);
    }

    void unbound(byte[] payload) {
        ByteBuffer buffer = ByteBuffer.wrap(payload);
        require(buffer, 3, "Actor unbound control");
        int version = Byte.toUnsignedInt(buffer.get());
        int slot = Short.toUnsignedInt(buffer.getShort());
        if (version != VERSION || slot == 0 || buffer.hasRemaining()) {
            throw new IllegalArgumentException("Actor unbound control payload is invalid");
        }
        DefaultActor actor;
        synchronized (this) {
            actor = bySlot.remove(slot);
            if (actor == null) {
                throw new IllegalArgumentException("Actor unbound control uses an unknown slot");
            }
            byId.remove(actor.actorId());
        }
        publishUnbound(actor);
    }

    void connectionEnded() {
        List<DefaultActor> actors;
        synchronized (this) {
            actors = new ArrayList<>(bySlot.values());
            bySlot.clear();
            byId.clear();
            actors.forEach(DefaultActor::close);
        }
        actors.forEach(actor -> publish(unboundHandlers, actor));
    }

    AutoCloseable onBound(ZLinkStreamActorHandler handler) {
        Objects.requireNonNull(handler, "handler");
        boundHandlers.add(handler);
        return () -> boundHandlers.remove(handler);
    }

    AutoCloseable onUnbound(ZLinkStreamActorHandler handler) {
        Objects.requireNonNull(handler, "handler");
        unboundHandlers.add(handler);
        return () -> unboundHandlers.remove(handler);
    }

    private void publish(List<ZLinkStreamActorHandler> handlers, DefaultActor actor) {
        for (ZLinkStreamActorHandler handler : handlers) {
            Runnable invoke = () -> invoke(handler, actor);
            if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
                invoke.run();
            } else {
                dispatchQueue.add(invoke);
            }
        }
    }

    private void publishUnbound(DefaultActor actor) {
        List<ZLinkStreamActorHandler> handlers = List.copyOf(unboundHandlers);
        Runnable invoke =
                () -> {
                    actor.close();
                    handlers.forEach(handler -> invoke(handler, actor));
                };
        if (configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE) {
            invoke.run();
        } else {
            dispatchQueue.add(invoke);
        }
    }

    private void invoke(ZLinkStreamActorHandler handler, DefaultActor actor) {
        try {
            handler.handle(actor)
                    .exceptionally(
                            failure -> {
                                errorPublisher.accept(
                                        DefaultZLinkStreamConnector.userCallbackFailed(failure));
                                return null;
                            });
        } catch (Throwable failure) {
            errorPublisher.accept(DefaultZLinkStreamConnector.userCallbackFailed(failure));
        }
    }

    private static String decodeActorId(byte[] encoded) {
        try {
            return StandardCharsets.UTF_8
                    .newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(encoded))
                    .toString();
        } catch (CharacterCodingException invalid) {
            throw new IllegalArgumentException("Actor id is not valid UTF-8", invalid);
        }
    }

    private static void require(ByteBuffer buffer, int size, String name) {
        if (buffer.remaining() < size) {
            throw new IllegalArgumentException(name + " payload is incomplete");
        }
    }

    static final class DefaultActor implements ZLinkStreamActor {
        private final DefaultZLinkStreamConnector connector;
        private final int slot;
        private final String actorId;
        private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>>
                handlers = new java.util.concurrent.ConcurrentHashMap<>();
        private volatile boolean bound = true;

        private DefaultActor(DefaultZLinkStreamConnector connector, int slot, String actorId) {
            this.connector = connector;
            this.slot = slot;
            this.actorId = actorId;
        }

        int slot() {
            return slot;
        }

        List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> handlers(String name) {
            return handlers.getOrDefault(name, List.of());
        }

        void close() {
            bound = false;
            handlers.clear();
        }

        @Override
        public String actorId() {
            return actorId;
        }

        @Override
        public boolean isBound() {
            return bound;
        }

        @Override
        public ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload) {
            return connector.actorSend(this, payload);
        }

        @Override
        public ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload) {
            return connector.actorRequest(this, payload);
        }

        @Override
        public ZLinkTypedStreamSendCall send(Object payload) {
            return new ZLinkTypedStreamConnectorSendCall(
                    send(connector.encodeActorPayload(payload)));
        }

        @Override
        public ZLinkTypedStreamRequestCall request(Object payload) {
            return new ZLinkTypedStreamConnectorRequestCall(
                    request(connector.encodeActorPayload(payload)));
        }

        @Override
        public AutoCloseable on(
                String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler) {
            DefaultZLinkStreamConnector.validatePacketName(name);
            Objects.requireNonNull(handler, "handler");
            handlers.computeIfAbsent(name, ignored -> new CopyOnWriteArrayList<>()).add(handler);
            return () -> handlers.getOrDefault(name, List.of()).remove(handler);
        }

        @Override
        public <TPayload> AutoCloseable on(
                Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler) {
            Objects.requireNonNull(payloadType, "payloadType");
            Objects.requireNonNull(handler, "handler");
            return on(
                    connector.options().nameResolver().resolve(payloadType), payloadType, handler);
        }

        @Override
        public <TPayload> AutoCloseable on(
                String name,
                Class<TPayload> payloadType,
                ZLinkStreamMessageHandler<TPayload> handler) {
            Objects.requireNonNull(payloadType, "payloadType");
            Objects.requireNonNull(handler, "handler");
            ZLinkStreamTypedCodec codec = connector.options().typedCodec();
            if (codec == null) {
                throw ZLinkStreamException.configurationError(
                        "typed stream payload API requires ZLinkStreamConnectorOptions.typedCodec");
            }
            return on(
                    name,
                    message ->
                            handler.handleAsync(
                                    new ZLinkStreamMessage<>(
                                            message.packetName(),
                                            codec.decode(message.payload(), payloadType),
                                            message.metadata(),
                                            message.flowId(),
                                            message.flowOrigin(),
                                            message.actorId())));
        }
    }
}
