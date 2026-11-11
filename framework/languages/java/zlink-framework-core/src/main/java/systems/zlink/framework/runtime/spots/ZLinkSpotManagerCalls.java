package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotCreateCall;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotGetOrCreateCall;
import systems.zlink.framework.spots.ZLinkSpotManager;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;

public final class ZLinkSpotManagerCalls implements ZLinkSpotManager {
    private final Supplier<ZLinkSpotRuntime> access;

    public ZLinkSpotManagerCalls(Supplier<ZLinkSpotRuntime> access) {
        this.access = access;
    }

    public ZLinkSpotCreateCall create(String type) {
        ZLinkSpotRuntime.rejectAfterRelocationReady("User Spot create");
        return new CreateCall(ZLinkSpotRuntime.requireStableType(type));
    }

    public ZLinkSpotGetOrCreateCall getOrCreate(String id, String type) {
        ZLinkSpotRuntime.rejectAfterRelocationReady("User Spot getOrCreate");
        return new GetOrCreateCall(
                systems.zlink.framework.runtime.internal.spots.ZLinkSpotIdValidator
                        .requireCallerAssignable(id),
                ZLinkSpotRuntime.requireStableType(type));
    }

    public CompletionStage<Optional<SpotRef>> find(String id) {
        return terminal(live -> live.find(id));
    }

    public CompletionStage<Boolean> close(SpotRef spot) {
        return terminal(live -> live.close(spot));
    }

    private <T> CompletionStage<T> terminal(
            java.util.function.Function<ZLinkSpotRuntime, CompletionStage<T>> operation) {
        try {
            return operation.apply(access.get());
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private abstract class UserSpotCall implements Runnable {
        private ZLinkSpotRuntime runtime;

        @Override
        public void run() {
            runtime = access.get();
        }

        final String stableType;
        String meshName;
        ZLinkMessage request = ZLinkMessage.empty();
        boolean requestSet;
        Duration timeout;
        final AtomicBoolean submitted = new AtomicBoolean();

        UserSpotCall(String stableType) {
            this.stableType = stableType;
        }

        void setMesh(String value) {
            if (meshName != null) {
                throw new IllegalStateException("inMesh was already set");
            }
            meshName = ZLinkSpotRuntime.requireStableType(value);
        }

        void setRequest(ZLinkMessage value) {
            if (requestSet) {
                throw new IllegalStateException("request was already set");
            }
            request = Objects.requireNonNull(value, "request");
            requestSet = true;
        }

        void setTimeout(Duration value) {
            if (timeout != null) {
                throw new IllegalStateException("timeout was already set");
            }
            if (value == null || value.isZero() || value.isNegative()) {
                throw new IllegalArgumentException("timeout must be positive");
            }
            timeout = value;
        }

        CompletionStage<ZLinkSpotCreateResult> submit(String spotId, boolean getOrCreate) {
            CompletionStage<ZLinkSpotCreateResult> failure =
                    systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls.beginOneWay(
                            submitted, this);
            if (failure != null) return failure;
            return runtime.submitUserSpot(
                    spotId, stableType, meshName, request, timeout, getOrCreate);
        }
    }

    private final class CreateCall extends UserSpotCall implements ZLinkSpotCreateCall {
        CreateCall(String stableType) {
            super(stableType);
        }

        @Override
        public CreateCall inMesh(String value) {
            setMesh(value);
            return this;
        }

        @Override
        public CreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }

        @Override
        public CreateCall request(ZLinkMessage value) {
            setRequest(value);
            return this;
        }

        @Override
        public CreateCall timeout(Duration value) {
            setTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResult> submit() {
            return submit(null, false);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResult> yield() {
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                    .requireYieldAllowed("Spot creation");
            return ZLinkSerialExecutionQueue.yieldCurrent(submit());
        }
    }

    private final class GetOrCreateCall extends UserSpotCall implements ZLinkSpotGetOrCreateCall {
        private final String spotId;

        GetOrCreateCall(String spotId, String stableType) {
            super(stableType);
            this.spotId = spotId;
        }

        @Override
        public GetOrCreateCall inMesh(String value) {
            setMesh(value);
            return this;
        }

        @Override
        public GetOrCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }

        @Override
        public GetOrCreateCall request(ZLinkMessage value) {
            setRequest(value);
            return this;
        }

        @Override
        public GetOrCreateCall timeout(Duration value) {
            setTimeout(value);
            return this;
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResult> submit() {
            return submit(spotId, true);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResult> yield() {
            systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext
                    .requireYieldAllowed("Spot creation");
            return ZLinkSerialExecutionQueue.yieldCurrent(submit());
        }
    }
}
