package systems.zlink.framework.runtime.internal.dispatch;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Consumer;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.sockets.ReceiveFlowState;
import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;

/** Applies the host queue's absolute pressure state to paired sockets. */
public final class ZLinkApplicationJobReceiveFlowController
    implements AutoCloseable {
    private static final Logger LOGGER = Logger.getLogger(
        ZLinkApplicationJobReceiveFlowController.class.getName());

    private final Object lock = new Object();
    private final ZLinkApplicationJobQueue queue;
    private final Map<Consumer<ReceiveFlowState>, Target> targets =
        new IdentityHashMap<>();
    private boolean closed;

    ZLinkApplicationJobReceiveFlowController(ZLinkApplicationJobQueue queue) {
        this.queue = Objects.requireNonNull(queue, "queue");
    }

    /** Registers a paired socket and applies the current absolute state. */
    public Registration register(Consumer<ReceiveFlowState> setter) {
        Objects.requireNonNull(setter, "setter");
        Target target = new Target(setter);
        synchronized (lock) {
            if (closed) {
                throw closedFailure();
            }
            targets.put(setter, target);
            target.desired = queue.pressureSnapshot();
        }
        try {
            target.apply(true);
            synchronized (lock) {
                if (closed || target.closing
                    || targets.get(setter) != target) {
                    throw closedFailure();
                }
            }
            return target;
        } catch (RuntimeException failure) {
            target.close();
            throw failure;
        }
    }

    void onPressureTransition(
        ZLinkApplicationJobQueue.PressureSnapshot snapshot) {
        List<Target> current;
        synchronized (lock) {
            if (closed) {
                return;
            }
            for (Target target : targets.values()) {
                if (target.desired == null
                    || snapshot.sequence() > target.desired.sequence()) {
                    target.desired = snapshot;
                }
            }
            current = new ArrayList<>(targets.values());
        }
        current.forEach(target -> target.apply(false));
    }

    void beginClose() {
        synchronized (lock) {
            if (closed) {
                return;
            }
            closed = true;
            targets.values().forEach(Target::requestClose);
            targets.clear();
        }
    }

    @Override
    public void close() {
        beginClose();
    }

    private static IllegalStateException closedFailure() {
        return new IllegalStateException(
            "application job receive-flow controller is closed");
    }

    public interface Registration extends AutoCloseable {
        @Override
        void close();
    }

    private final class Target implements Registration {
        private final Consumer<ReceiveFlowState> setter;
        private final Object applyLock = new Object();
        private ZLinkApplicationJobQueue.PressureSnapshot desired;
        private long appliedSequence = -1;
        private volatile boolean closing;

        private Target(Consumer<ReceiveFlowState> setter) {
            this.setter = setter;
        }

        private void apply(boolean rethrowFailure) {
            synchronized (applyLock) {
                while (true) {
                    ZLinkApplicationJobQueue.PressureSnapshot next;
                    synchronized (lock) {
                        if (closing || closed || targets.get(setter) != this
                            || desired == null
                            || desired.sequence() <= appliedSequence) {
                            return;
                        }
                        next = desired;
                        appliedSequence = next.sequence();
                    }
                    try {
                        setter.accept(toBindingState(next.pressureState()));
                    } catch (RuntimeException failure) {
                        if (!closing || !isClosingInvalidState(failure)) {
                            queue.recordReceiveFlowConfigurationFailure();
                            LOGGER.log(Level.WARNING,
                                "Failed to apply application job receive-flow state",
                                failure);
                            if (rethrowFailure) {
                                throw failure;
                            }
                        }
                    }
                }
            }
        }

        @Override
        public void close() {
            requestClose();
            synchronized (lock) {
                targets.remove(setter, this);
            }
            synchronized (applyLock) {
                desired = null;
            }
        }

        private void requestClose() {
            closing = true;
        }
    }

    private static ReceiveFlowState toBindingState(
        ZLinkApplicationJobQueuePressureState state) {
        return state == ZLinkApplicationJobQueuePressureState.PAUSED
            ? ReceiveFlowState.PAUSED : ReceiveFlowState.RUNNING;
    }

    private static boolean isClosingInvalidState(RuntimeException failure) {
        return failure instanceof ZlinkConfigException config
            && config.getResult() == ConfigResult.INVALID_STATE;
    }
}
