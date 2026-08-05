package systems.zlink.framework.runtime.internal.handlers;

import java.util.Objects;
import java.util.function.Predicate;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/** Internal handler state that coroutine adapters must restore on every resume. */
public final class ZLinkSuspendInvocationContext {
    private static final ThreadLocal<Object> ENTRY_SPOT_DISPATCH = new ThreadLocal<>();
    private static final ThreadLocal<Object> SPOT_OUTBOUND = new ThreadLocal<>();
    private static final ThreadLocal<String> ACTOR_DISPATCH = new ThreadLocal<>();
    private static final ThreadLocal<Object> DEFERRED_ACTOR_JOIN = new ThreadLocal<>();
    private static final ThreadLocal<Object> SERIAL_EXECUTION_TURN = new ThreadLocal<>();
    private static final ThreadLocal<ApplicationExecution> APPLICATION_EXECUTION =
        new ThreadLocal<>();

    private ZLinkSuspendInvocationContext() {
    }

    public static Object currentEntrySpotDispatch() {
        return ENTRY_SPOT_DISPATCH.get();
    }

    public static ThreadLocal<Object> entrySpotDispatchThreadLocal() {
        return ENTRY_SPOT_DISPATCH;
    }

    public static Object currentSpotOutbound() {
        return SPOT_OUTBOUND.get();
    }

    public static ThreadLocal<Object> spotOutboundThreadLocal() {
        return SPOT_OUTBOUND;
    }

    public static String currentActorDispatch() {
        return ACTOR_DISPATCH.get();
    }

    public static ThreadLocal<String> actorDispatchThreadLocal() {
        return ACTOR_DISPATCH;
    }

    public static Object currentDeferredActorJoin() {
        return DEFERRED_ACTOR_JOIN.get();
    }

    public static ThreadLocal<Object> deferredActorJoinThreadLocal() {
        return DEFERRED_ACTOR_JOIN;
    }

    public static Object currentSerialExecutionTurn() {
        return SERIAL_EXECUTION_TURN.get();
    }

    public static ThreadLocal<Object> serialExecutionTurnThreadLocal() {
        return SERIAL_EXECUTION_TURN;
    }

    public static ApplicationExecution currentApplicationExecution() {
        return APPLICATION_EXECUTION.get();
    }

    public static ThreadLocal<ApplicationExecution> applicationExecutionThreadLocal() {
        return APPLICATION_EXECUTION;
    }

    public static Scope enterEntrySpotDispatch(Object context) {
        Object previous = ENTRY_SPOT_DISPATCH.get();
        if (context == null) {
            ENTRY_SPOT_DISPATCH.remove();
        } else {
            ENTRY_SPOT_DISPATCH.set(context);
        }
        return () -> {
            if (previous == null) {
                ENTRY_SPOT_DISPATCH.remove();
            } else {
                ENTRY_SPOT_DISPATCH.set(previous);
            }
        };
    }

    public static Scope enterSpotOutbound(Object outbound) {
        return enter(SPOT_OUTBOUND, outbound);
    }

    public static Scope enterActorDispatch(String actorId) {
        return enter(ACTOR_DISPATCH, actorId);
    }

    public static Scope enterSerialExecutionTurn(Object turn) {
        return enter(SERIAL_EXECUTION_TURN, turn);
    }

    public static Scope enterApplicationExecution(ApplicationExecution execution) {
        return enter(APPLICATION_EXECUTION, execution);
    }

    public static void requireYieldAllowed(String operation) {
        ApplicationExecution execution = APPLICATION_EXECUTION.get();
        if (execution == null || !execution.yieldAllowed()) {
            throw invalid(
                operation + " yield is only valid in a SpotWide User Spot "
                    + "or Instance Spot application callback");
        }
    }

    public static <T> T rejectYield(String operation) {
        throw invalid(operation + " does not support yield");
    }

    public static void rejectAfterRelocationReady(String operation) {
        ApplicationExecution execution = APPLICATION_EXECUTION.get();
        if (execution != null && execution.relocationReadyDeferred()) {
            throw invalid(
                operation + " cannot start after relocationReady().defer() "
                    + "in the same Spot turn");
        }
    }

    public static void rejectSameSpotWait(String targetSpotId) {
        ApplicationExecution execution = APPLICATION_EXECUTION.get();
        if (execution != null
            && execution.sharedSpotGate()
            && execution.spotId().equals(targetSpotId)) {
            throw invalid(
                "Awaiting a request to the current Spot would wait on the "
                    + "same execution gate");
        }
    }

    public static void rejectSameActorWait(String targetActorId) {
        ApplicationExecution execution = APPLICATION_EXECUTION.get();
        if (execution == null || !execution.sharedSpotGate()) {
            return;
        }
        if (targetActorId.equals(execution.actorId())
            || execution.memberActor().test(targetActorId)) {
            throw invalid(
                "Awaiting a request to the current or another member Actor "
                    + "would wait on the same User Spot gate");
        }
    }

    public static void rejectCurrentActorJoinWait(String actorId) {
        ApplicationExecution execution = APPLICATION_EXECUTION.get();
        if (execution != null
            && execution.sharedSpotGate()
            && actorId != null
            && actorId.equals(execution.actorId())) {
            throw invalid(
                "Awaiting the current Actor join would wait on the same "
                    + "User Spot gate");
        }
    }

    private static ZLinkFrameworkException invalid(String message) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NOT_CONFIGURED,
            message);
    }

    private static <T> Scope enter(ThreadLocal<T> local, T value) {
        T previous = local.get();
        if (value == null) {
            local.remove();
        } else {
            local.set(value);
        }
        return () -> {
            if (previous == null) {
                local.remove();
            } else {
                local.set(previous);
            }
        };
    }

    @FunctionalInterface
    public interface Scope extends AutoCloseable {
        @Override
        void close();
    }

    public static final class ApplicationExecution {
        private final String spotId;
        private final String actorId;
        private final boolean sharedSpotGate;
        private final boolean yieldAllowed;
        private final boolean relocationReadyAllowed;
        private final Predicate<String> memberActor;
        private final java.util.concurrent.atomic.AtomicBoolean
            relocationReadyDeferred =
                new java.util.concurrent.atomic.AtomicBoolean();

        public ApplicationExecution(
            String spotId,
            String actorId,
            boolean sharedSpotGate,
            boolean yieldAllowed,
            Predicate<String> memberActor) {
            this(
                spotId,
                actorId,
                sharedSpotGate,
                yieldAllowed,
                false,
                memberActor);
        }

        public ApplicationExecution(
            String spotId,
            String actorId,
            boolean sharedSpotGate,
            boolean yieldAllowed,
            boolean relocationReadyAllowed,
            Predicate<String> memberActor) {
            this.spotId = Objects.requireNonNull(spotId, "spotId");
            this.actorId = actorId;
            this.sharedSpotGate = sharedSpotGate;
            this.yieldAllowed = yieldAllowed;
            this.relocationReadyAllowed = relocationReadyAllowed;
            this.memberActor =
                memberActor == null ? ignored -> false : memberActor;
        }

        public String spotId() {
            return spotId;
        }

        public String actorId() {
            return actorId;
        }

        public boolean sharedSpotGate() {
            return sharedSpotGate;
        }

        public boolean yieldAllowed() {
            return yieldAllowed;
        }

        public Predicate<String> memberActor() {
            return memberActor;
        }

        public boolean relocationReadyAllowed() {
            return relocationReadyAllowed;
        }

        public boolean relocationReadyDeferred() {
            return relocationReadyDeferred.get();
        }

        public boolean tryDeferRelocationReady() {
            return relocationReadyDeferred.compareAndSet(false, true);
        }
    }
}
