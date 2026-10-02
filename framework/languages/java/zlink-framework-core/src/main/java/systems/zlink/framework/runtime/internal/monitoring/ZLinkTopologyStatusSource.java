package systems.zlink.framework.runtime.internal.monitoring;

import systems.zlink.framework.monitoring.ZLinkObservedStatus;

import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Flow;
import java.util.function.BiFunction;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.ToLongFunction;

/** One source owns the published payload and sequence shared by queries and observers. */
public final class ZLinkTopologyStatusSource<T> {
    private final ConcurrentHashMap<String, Source> sources = new ConcurrentHashMap<>();
    private final BiFunction<String, T, T> snapshot;
    private final Function<T, Object> fingerprint;
    private final BiFunction<T, Long, T> withSequence;
    private final ToLongFunction<T> sequence;
    private final Predicate<T> terminal;
    private final Predicate<T> preserve;

    public ZLinkTopologyStatusSource(
            BiFunction<String, T, T> snapshot,
            Function<T, Object> fingerprint,
            BiFunction<T, Long, T> withSequence,
            ToLongFunction<T> sequence,
            Predicate<T> terminal,
            Predicate<T> preserve) {
        this.snapshot = snapshot;
        this.fingerprint = fingerprint;
        this.withSequence = withSequence;
        this.sequence = sequence;
        this.terminal = terminal;
        this.preserve = preserve;
    }

    public T snapshot(String sourceKey) {
        Source source = source(sourceKey);
        T published = source.status;
        source.publisher.signalIfSubscribed();
        return published;
    }

    public Flow.Publisher<ZLinkObservedStatus<T>> observe(String sourceKey, int capacity) {
        if (capacity < ZLinkStatusPublisher.MINIMUM_CAPACITY) {
            throw new IllegalArgumentException("capacity must be positive");
        }
        Source source = source(sourceKey);
        return subscriber -> source.publisher.subscribe(subscriber, capacity);
    }

    /** Signals the existing dispatcher; it never queries a runtime lane on the signaling thread. */
    public void signalAll() {
        sources.values().forEach(source -> source.publisher.signal());
    }

    public boolean isTerminal(String sourceKey) {
        Source source = sources.get(sourceKey);
        if (source == null) {
            return false;
        }
        T published = source.status;
        return terminal.test(published);
    }

    public void signal(String sourceKey) {
        Source source = sources.get(sourceKey);
        if (source != null) {
            source.publisher.signal();
        }
    }

    private Source source(String sourceKey) {
        return sources.compute(
                sourceKey,
                (key, previous) -> {
                    Source current = previous == null ? new Source(key) : previous;
                    current.publishSnapshot();
                    return current;
                });
    }

    private final class Source {
        private final String key;
        private volatile T status;
        private final ZLinkStatusPublisher<T> publisher;

        private Source(String key) {
            this.key = key;
            status = snapshot.apply(key, null);
            publisher =
                    ZLinkStatusPublisher.create(
                            () -> source(key).status,
                            fingerprint,
                            ignored -> key,
                            ZLinkStatusPublisher.MINIMUM_CAPACITY,
                            terminal,
                            preserve);
        }

        /** Called only inside this source key's existing map compute turn. */
        private void publishSnapshot() {
            long previousSequence = sequence.applyAsLong(status);
            if (previousSequence != 0 && terminal.test(status)) {
                return;
            }
            T current = previousSequence == 0 ? status : snapshot.apply(key, status);
            if (previousSequence == 0
                    || !Objects.equals(fingerprint.apply(status), fingerprint.apply(current))) {
                status = withSequence.apply(current, Math.addExact(previousSequence, 1));
            }
        }
    }
}
