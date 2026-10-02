package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.LongSupplier;

final class ZLinkAutoConnectReconciler {
    private final ZLinkAutoConnectPlanner.Local local;
    private final ZLinkAutoConnectPeerResolver peers;
    private final ZLinkAutoConnectExecutor executor;
    private final ZLinkLocationOptions options;
    private final LongSupplier nanoTime;
    private final Consumer<? super ZlinkConnectException> runtimeErrorSink;
    private final Map<String, ZLinkAutoConnectPlanner.Target> active = new HashMap<>();
    private final Map<String, ZLinkAutoConnectPlanner.Target> notRequired = new HashMap<>();
    private Map<String, ZLinkAutoConnectPlanner.Target> lastDesired = Map.of();
    private final Map<String, ZLinkAutoConnectPlanner.Target> observedManual = new HashMap<>();
    private final Map<String, ZLinkAutoConnectPlanner.Target> admissionExpectations =
            new HashMap<>();
    private boolean storeFailed;
    private long storeFailureStartedNanos = -1;
    private long recoveryDeferUntilNanos;

    ZLinkAutoConnectReconciler(
            ZLinkAutoConnectPlanner.Local local,
            ZLinkAutoConnectPeerResolver peers,
            ZLinkAutoConnectExecutor executor,
            ZLinkLocationOptions options,
            LongSupplier nanoTime,
            Consumer<? super ZlinkConnectException> runtimeErrorSink) {
        this.local = Objects.requireNonNull(local, "local");
        this.peers = Objects.requireNonNull(peers, "peers");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.options = Objects.requireNonNull(options, "options");
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
        this.runtimeErrorSink = Objects.requireNonNull(runtimeErrorSink, "runtimeErrorSink");
    }

    boolean storeFailed() {
        return storeFailed;
    }

    CompletionStage<Void> tick() {
        return peers.listPeers(local.type(), local.meshName(), null)
                .handle(
                        (rows, failure) -> {
                            if (failure != null) {
                                if (!storeFailed) storeFailureStartedNanos = nanoTime.getAsLong();
                                storeFailed = true;
                                retryPendingTargetsWithinStoreFailureGrace();
                            } else {
                                reconcile(rows);
                            }
                            return null;
                        });
    }

    CompletionStage<Void> shutdown() {
        active.values().forEach(target -> attemptConnection(() -> executor.disconnect(target)));
        notRequired.values().forEach(executor::clearNotRequired);
        admissionExpectations.values().forEach(executor::forgetAdmissionExpectation);
        active.clear();
        notRequired.clear();
        admissionExpectations.clear();
        return CompletableFuture.completedFuture(null);
    }

    CompletionStage<Void> markDraining() {
        return CompletableFuture.completedFuture(null);
    }

    private void reconcile(List<ZLinkAutoConnectPeer> rows) {
        if (storeFailed) {
            storeFailed = false;
            storeFailureStartedNanos = -1;
            recoveryDeferUntilNanos = nanoTime.getAsLong() + options.ownerLeaseTtl().toNanos();
        }
        Map<String, ZLinkAutoConnectPlanner.Target> desired =
                ZLinkAutoConnectPlanner.computeDesired(local, rows);
        if (nanoTime.getAsLong() < recoveryDeferUntilNanos) {
            lastDesired.forEach(desired::putIfAbsent);
        }
        Map<String, ZLinkAutoConnectPlanner.Target> nextNotRequired =
                ZLinkAutoConnectPlanner.computeNotRequired(local, rows);
        Map<String, ZLinkAutoConnectPlanner.Target> nextExpectations = new HashMap<>();
        for (ZLinkAutoConnectPeer row : rows) {
            ZLinkAutoConnectPlanner.Target target =
                    ZLinkAutoConnectPlanner.trackableTarget(local, row);
            if (target != null && ZLinkAutoConnectPlanner.hasRid(target.nodeRid())) {
                nextExpectations.put(target.nodeRid().toHex(), target);
            }
        }
        admissionExpectations.forEach(
                (key, target) -> {
                    if (!nextExpectations.containsKey(key)) {
                        executor.forgetAdmissionExpectation(target);
                    }
                });
        nextExpectations.forEach(
                (key, target) -> {
                    if (!target.equals(admissionExpectations.get(key))) {
                        executor.observeAdmissionExpectation(target);
                    }
                });
        admissionExpectations.clear();
        admissionExpectations.putAll(nextExpectations);
        lastDesired = Map.copyOf(desired);
        notRequired.keySet().stream()
                .filter(key -> !nextNotRequired.containsKey(key))
                .map(notRequired::get)
                .forEach(executor::clearNotRequired);
        nextNotRequired.forEach(
                (key, target) -> {
                    ZLinkAutoConnectPlanner.Target current = notRequired.get(key);
                    if (current == null
                            || !current.endpoint().equals(target.endpoint())
                            || current.lifecycleGeneration() != target.lifecycleGeneration()) {
                        if (current != null) {
                            executor.clearNotRequired(current);
                        }
                        executor.markNotRequired(target);
                    }
                });
        notRequired.clear();
        notRequired.putAll(nextNotRequired);
        Map<String, ZLinkAutoConnectPlanner.Target> manualSnapshot = new HashMap<>();
        for (ZLinkAutoConnectPeer row : rows) {
            ZLinkAutoConnectPlanner.Target target =
                    ZLinkAutoConnectPlanner.trackableTarget(local, row);
            if (target == null || !executor.isManual(target) || desired.containsKey(target.key()))
                continue;
            ZLinkAutoConnectPlanner.Target previous =
                    observedManual.values().stream()
                            .filter(candidate -> samePeerIdentity(candidate, target))
                            .findFirst()
                            .orElse(null);
            if (previous != null
                    && (!previous.key().equals(target.key())
                            || !previous.endpoint().equals(target.endpoint())
                            || !Objects.equals(previous.ownerId(), target.ownerId()))) {
                if (!attemptConnection(() -> executor.replace(previous, target))) continue;
                observedManual.remove(previous.key());
            }
            manualSnapshot.put(target.key(), target);
        }
        observedManual.putAll(manualSnapshot);
        List<String> toRemove = new ArrayList<>();
        for (Map.Entry<String, ZLinkAutoConnectPlanner.Target> entry : desired.entrySet()) {
            ZLinkAutoConnectPlanner.Target current = active.get(entry.getKey());
            ZLinkAutoConnectPlanner.Target target = entry.getValue();
            if (current == null) {
                if (attemptConnection(() -> executor.connect(target)))
                    active.put(entry.getKey(), target);
            } else if ((!current.endpoint().equals(target.endpoint())
                            || !Objects.equals(current.ownerId(), target.ownerId()))
                    && attemptConnection(() -> executor.replace(current, target))) {
                active.put(entry.getKey(), target);
            }
        }
        active.keySet().stream().filter(key -> !desired.containsKey(key)).forEach(toRemove::add);
        toRemove.forEach(
                key -> {
                    ZLinkAutoConnectPlanner.Target target = active.get(key);
                    if (attemptConnection(() -> executor.disconnect(target))) active.remove(key);
                });
    }

    private boolean attemptConnection(BooleanSupplier operation) {
        try {
            return operation.getAsBoolean();
        } catch (ZlinkConnectException failure) {
            runtimeErrorSink.accept(failure);
            return false;
        }
    }

    private static boolean samePeerIdentity(
            ZLinkAutoConnectPlanner.Target left, ZLinkAutoConnectPlanner.Target right) {
        return ZLinkAutoConnectPlanner.hasRid(left.nodeRid())
                        && ZLinkAutoConnectPlanner.hasRid(right.nodeRid())
                ? left.role() == right.role() && left.nodeRid().equals(right.nodeRid())
                : left.role() == right.role() && left.endpoint().equals(right.endpoint());
    }

    private void retryPendingTargetsWithinStoreFailureGrace() {
        if (storeFailureStartedNanos < 0
                || options.storeFailureGrace().isZero()
                || options.storeFailureGrace().isNegative()
                || nanoTime.getAsLong() - storeFailureStartedNanos
                        > options.storeFailureGrace().toNanos()) return;
        lastDesired.forEach(
                (key, target) -> {
                    if (!active.containsKey(key)
                            && attemptConnection(() -> executor.connect(target)))
                        active.put(key, target);
                });
    }
}
