package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

public final class ZLinkActorManagerCalls implements ZLinkActorManager {
    private final Supplier<ZLinkActorRuntime> access;
    private final Duration defaultTimeout;

    public ZLinkActorManagerCalls(Supplier<ZLinkActorRuntime> access, Duration defaultTimeout) {
        this.access = access;
        this.defaultTimeout = defaultTimeout;
    }

    public ZLinkActorCreateCall create(String actorId, String actorType) {
        ZLinkActorRuntime.rejectAfterRelocationReady("Actor create");
        return new ActorCreateCall(actorId, actorType);
    }

    public ZLinkActorGetOrCreateCall getOrCreate(String actorId, String actorType) {
        ZLinkActorRuntime.rejectAfterRelocationReady("Actor getOrCreate");
        return new ActorGetOrCreateCall(actorId, actorType);
    }

    public CompletionStage<Optional<ActorRef>> find(String id) {
        return terminal(live -> live.find(id));
    }

    public CompletionStage<Optional<systems.zlink.framework.spots.SpotRef>> findSpot(String id) {
        return terminal(live -> live.findSpot(id));
    }

    public CompletionStage<Boolean> destroy(ActorRef actor) {
        return terminal(live -> live.destroy(actor));
    }

    private <T> CompletionStage<T> terminal(
            java.util.function.Function<ZLinkActorRuntime, CompletionStage<T>> operation) {
        try {
            return operation.apply(access.get());
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private abstract class ActorCreationCall implements Runnable {
        private ZLinkActorRuntime runtime;

        @Override
        public void run() {
            runtime = access.get();
        }

        private final String actorId;
        private final String actorType;
        private final boolean getOrCreate;
        private final AtomicBoolean submitted = new AtomicBoolean();
        private String selectedMesh;
        private ZLinkMessage request = ZLinkMessage.empty();
        private boolean requestSet;
        private Duration timeout = defaultTimeout;
        private boolean timeoutSet;

        ActorCreationCall(String actorId, String actorType, boolean getOrCreate) {
            this.actorId = actorId;
            this.actorType = actorType;
            this.getOrCreate = getOrCreate;
        }

        final void setMesh(String value) {
            if (selectedMesh != null) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION, "Mesh was already set");
            }
            if (value == null || value.isBlank()) {
                throw new IllegalArgumentException("meshName is required");
            }
            selectedMesh = value;
        }

        final void setRequest(ZLinkMessage value) {
            if (requestSet) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION, "request was already set");
            }
            request = Objects.requireNonNull(value, "request");
            requestSet = true;
        }

        final void setTimeout(Duration value) {
            if (timeoutSet) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION, "timeout was already set");
            }
            if (value == null || value.isZero() || value.isNegative()) {
                throw new IllegalArgumentException("timeout must be positive");
            }
            timeout = value;
            timeoutSet = true;
        }

        final CompletionStage<ZLinkActorCreateResult> submitOperation() {
            CompletionStage<ZLinkActorCreateResult> failure =
                    systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.beginOneWay(
                            submitted, this);
            if (failure != null) return failure;
            return runtime.submitCreate(
                    actorId, actorType, request, getOrCreate, selectedMesh, timeout);
        }

        final CompletionStage<ZLinkActorCreateResult> yieldOperation() {
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                    .requireYieldAllowed("Actor creation");
            return ZLinkSerialExecutionQueue.yieldCurrent(submitOperation());
        }
    }

    private final class ActorCreateCall extends ActorCreationCall implements ZLinkActorCreateCall {
        ActorCreateCall(String actorId, String actorType) {
            super(actorId, actorType, false);
        }

        @Override
        public ActorCreateCall inMesh(String value) {
            setMesh(value);
            return this;
        }

        @Override
        public ActorCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }

        @Override
        public ActorCreateCall request(ZLinkMessage value) {
            setRequest(value);
            return this;
        }

        @Override
        public ActorCreateCall timeout(Duration value) {
            setTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<ZLinkActorCreateResult> submit() {
            return submitOperation();
        }

        @Override
        public CompletionStage<ZLinkActorCreateResult> yield() {
            return yieldOperation();
        }
    }

    private final class ActorGetOrCreateCall extends ActorCreationCall
            implements ZLinkActorGetOrCreateCall {
        ActorGetOrCreateCall(String actorId, String actorType) {
            super(actorId, actorType, true);
        }

        @Override
        public ActorGetOrCreateCall inMesh(String value) {
            setMesh(value);
            return this;
        }

        @Override
        public ActorGetOrCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }

        @Override
        public ActorGetOrCreateCall request(ZLinkMessage value) {
            setRequest(value);
            return this;
        }

        @Override
        public ActorGetOrCreateCall timeout(Duration value) {
            setTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<ZLinkActorCreateResult> submit() {
            return submitOperation();
        }

        @Override
        public CompletionStage<ZLinkActorCreateResult> yield() {
            return yieldOperation();
        }
    }
}
