package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.channels.ZLinkRequestStage;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.calls.ZLinkRequestStages;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRequestMetrics;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

public final class ZLinkActorClientCalls implements ZLinkActorClient {
    private final Supplier<ZLinkActorClientRuntime> access;

    public ZLinkActorClientCalls(Supplier<ZLinkActorClientRuntime> access) {
        this.access = access;
    }

    @Override
    public ZLinkActorSendCall sendToActor(String actorId, Object message) {
        return new SendCall(actorId, message);
    }

    @Override
    public ZLinkActorRequestCall requestToActor(String actorId, Object message) {
        return new RequestCall(actorId, message);
    }

    private final class SendCall implements ZLinkActorSendCall, Runnable {
        private ZLinkActorClientRuntime runtime;

        @Override
        public void run() {
            runtime = access.get();
        }

        private final AtomicBoolean submitGate = new AtomicBoolean();
        private final String actorId;
        private final Object message;
        private String packetName;
        private final Map<String, String> metadata = new LinkedHashMap<>();

        SendCall(String actorId, Object message) {
            this.actorId = ZLinkActorClientRuntime.requireActorId(actorId);
            this.message = message;
            this.packetName = ZLinkPacketNames.resolve(message);
        }

        public ZLinkActorSendCall packetName(String packetName) {
            this.packetName = packetName;
            return this;
        }

        @Override
        public ZLinkActorSendCall metadata(String key, String value) {
            metadata.put(
                    Objects.requireNonNull(key, "key"), Objects.requireNonNull(value, "value"));
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate, this);
            if (duplicate != null) {
                return duplicate;
            }
            try (var flowScope = runtime.enterApplicationFlow()) {
                return runtime.submitSendResult(actorId, packetName, message, metadata);
            }
        }
    }

    private final class RequestCall implements ZLinkActorRequestCall, Runnable {
        private CompletableFuture<Void> requestAdmission;

        private final String actorId;
        private final Object request;
        private ZLinkActorClientRuntime runtime;

        @Override
        public void run() {
            runtime = access.get();
        }

        private final AtomicBoolean submitGate = new AtomicBoolean();
        private String packetName;
        private final Map<String, String> metadata = new LinkedHashMap<>();
        private Duration timeout;

        RequestCall(String actorId, Object request) {
            this.actorId = ZLinkActorClientRuntime.requireActorId(actorId);
            this.request = request;
            this.packetName = ZLinkPacketNames.resolve(request);
        }

        public ZLinkActorRequestCall packetName(String packetName) {
            this.packetName = packetName;
            return this;
        }

        @Override
        public ZLinkActorRequestCall metadata(String key, String value) {
            metadata.put(
                    Objects.requireNonNull(key, "key"), Objects.requireNonNull(value, "value"));
            return this;
        }

        @Override
        public ZLinkActorRequestCall timeout(Duration timeout) {
            this.timeout = timeout;
            return this;
        }

        @Override
        public <TReply> ZLinkRequestStage<TReply> submit(Class<TReply> replyType) {
            CompletionStage<TReply> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate, this);
            if (duplicate != null)
                return ZLinkRequestStages.observe(duplicate, new CompletableFuture<>());
            requestAdmission = new CompletableFuture<>();
            return ZLinkRequestStages.observe(submitReply(replyType), requestAdmission);
        }

        private <TReply> CompletionStage<TReply> submitReply(Class<TReply> replyType) {

            try (var flowScope = runtime.enterApplicationFlow()) {
                systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                        .rejectSameActorWait(actorId);
                ZLinkInternalSpotNode node = runtime.spotNode.get();
                ZLinkRequestMetrics.Series metric = ZLinkRequestMetrics.actor(node.name());
                long started =
                        ZLinkRequestMetrics.durationEnabled()
                                ? System.nanoTime()
                                : ZLinkRequestMetrics.NO_START;
                ZLinkRequestMetrics.start(metric);
                CompletionStage<TReply> stage;
                try {
                    stage =
                            runtime.requestAsync(
                                    actorId,
                                    packetName,
                                    request,
                                    metadata,
                                    timeout,
                                    replyType,
                                    metric,
                                    started,
                                    requestAdmission);
                } catch (RuntimeException | Error failure) {
                    ZLinkRequestMetrics.complete(
                            metric,
                            started == ZLinkRequestMetrics.NO_START
                                    ? -1L
                                    : ZLinkRequestMetrics.elapsed(started, System.nanoTime()),
                            failure);
                    throw failure;
                }
                return ZLinkSerialExecutionQueue.manageCurrent(stage);
            }
        }

        @Override
        public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                    .requireYieldAllowed("Actor request");
            return ZLinkSerialExecutionQueue.yieldCurrent(submit(replyType));
        }
    }
}
