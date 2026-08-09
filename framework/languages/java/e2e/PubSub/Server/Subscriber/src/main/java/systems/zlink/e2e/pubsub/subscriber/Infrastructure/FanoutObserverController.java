package Infrastructure;

import systems.zlink.e2e.pubsub.subscriber.Infrastructure;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Flow;
import java.util.concurrent.atomic.AtomicBoolean;
import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.framework.monitoring.ZLinkFanoutStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

public final class FanoutObserverController {
    private final ObjectProvider<ZLinkFrameworkRuntime> runtime;
    private final Object monitor = new Object();
    private final Map<String, ControlledObserver> observers = new LinkedHashMap<>();
    private final List<Entry> entries = new ArrayList<>();

    public FanoutObserverController(ObjectProvider<ZLinkFrameworkRuntime> runtime) {
        this.runtime = runtime;
    }

    public void start(String name, int capacity, boolean slow) {
        if (name == null || name.isBlank() || capacity <= 0) {
            throw new IllegalArgumentException("observer name and positive capacity are required");
        }
        cancel(name);
        ControlledObserver observer = new ControlledObserver(name, slow);
        synchronized (monitor) { observers.put(name, observer); }
        runtime.getObject().fanoutRuntime()
            .observe(Contracts.EVENT_CHANNEL, capacity).subscribe(observer);
    }

    public void release(String name) {
        synchronized (monitor) {
            ControlledObserver observer = observers.get(name);
            if (observer != null) observer.release();
        }
    }

    public void cancel(String name) {
        synchronized (monitor) {
            ControlledObserver observer = observers.remove(name);
            if (observer != null) observer.cancel();
        }
    }

    public void waitFor(String name, long timeoutMillis) {
        long deadline = System.nanoTime() + timeoutMillis * 1_000_000L;
        synchronized (monitor) {
            while (entries.stream().noneMatch(entry -> entry.observer().equals(name))
                && System.nanoTime() < deadline) {
                try {
                    monitor.wait(Math.max(1L, Math.min(250L,
                        (deadline - System.nanoTime()) / 1_000_000L)));
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("observer wait interrupted", error);
                }
            }
            if (entries.stream().noneMatch(entry -> entry.observer().equals(name))) {
                throw new IllegalStateException("observer " + name + " did not receive a status");
            }
        }
    }

    public List<Entry> snapshot() {
        synchronized (monitor) { return List.copyOf(entries); }
    }

    public record Entry(
        String observer,
        long sequence,
        String state,
        int readyPublisherCount,
        long lossCoalesced,
        long lossDiscardedTerminal) { }

    private final class ControlledObserver
        implements Flow.Subscriber<ZLinkObservedStatus<ZLinkFanoutStatus>> {
        private final String name;
        private final AtomicBoolean released;
        private Flow.Subscription subscription;

        private ControlledObserver(String name, boolean slow) {
            this.name = name;
            released = new AtomicBoolean(!slow);
        }

        @Override public void onSubscribe(Flow.Subscription value) {
            subscription = value;
            value.request(1);
        }

        @Override public void onNext(ZLinkObservedStatus<ZLinkFanoutStatus> value) {
            var status = value.status();
            synchronized (monitor) {
                entries.add(new Entry(
                    name, status.sequence(), status.state().name(),
                    status.readyPublisherCount(), value.loss().coalescedCount(),
                    value.loss().discardedTerminalCount()));
                monitor.notifyAll();
                while (!released.get()) {
                    try {
                        monitor.wait(250L);
                    } catch (InterruptedException error) {
                        Thread.currentThread().interrupt();
                        return;
                    }
                }
            }
            subscription.request(1);
        }

        @Override public void onError(Throwable error) { synchronized (monitor) { monitor.notifyAll(); } }
        @Override public void onComplete() { synchronized (monitor) { monitor.notifyAll(); } }

        private void release() {
            released.set(true);
            synchronized (monitor) { monitor.notifyAll(); }
        }

        private void cancel() {
            release();
            if (subscription != null) subscription.cancel();
        }
    }
}
