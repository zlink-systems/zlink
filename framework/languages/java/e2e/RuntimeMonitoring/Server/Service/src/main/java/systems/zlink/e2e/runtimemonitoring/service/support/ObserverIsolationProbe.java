package systems.zlink.e2e.runtimemonitoring.service.support;

import java.util.concurrent.Flow;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.e2e.runtimemonitoring.shared.Contracts;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;

public final class ObserverIsolationProbe {
    private final AtomicLong normalEventCount = new AtomicLong();
    private final AtomicLong normalLatestSequence = new AtomicLong();
    private final AtomicLong slowLatestSequence = new AtomicLong();
    private final AtomicBoolean slowFailed = new AtomicBoolean();
    private volatile Flow.Subscription slowSubscription;
    private volatile boolean started;

    public synchronized Contracts.ObserverIsolationStatus start(
        ZLinkRouteMeshRuntime runtime) {
        if (started) {
            return status();
        }
        started = true;
        runtime.observe(Contracts.SPOT_MESH, 2).subscribe(
            new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                @Override
                public void onSubscribe(Flow.Subscription subscription) {
                    subscription.request(Long.MAX_VALUE);
                }

                @Override
                public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                    ZLinkMeshNodeSnapshot status = observed.status();
                    normalEventCount.incrementAndGet();
                    normalLatestSequence.set(status.sequence());
                }

                @Override
                public void onError(Throwable error) {
                }

                @Override
                public void onComplete() {
                }
            });
        runtime.observe(Contracts.SPOT_MESH, 2).subscribe(
            new Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>>() {
                @Override
                public void onSubscribe(Flow.Subscription subscription) {
                    slowSubscription = subscription;
                }

                @Override
                public void onNext(ZLinkObservedStatus<ZLinkMeshNodeSnapshot> observed) {
                    ZLinkMeshNodeSnapshot status = observed.status();
                    slowLatestSequence.set(status.sequence());
                    throw new IllegalStateException("intentional slow observer failure");
                }

                @Override
                public void onError(Throwable error) {
                    slowFailed.set(true);
                }

                @Override
                public void onComplete() {
                }
            });
        return status();
    }

    public Contracts.ObserverIsolationStatus release() {
        Flow.Subscription subscription = slowSubscription;
        if (subscription != null) {
            subscription.request(1);
        }
        return status();
    }

    public Contracts.ObserverIsolationStatus status() {
        return new Contracts.ObserverIsolationStatus(
            started,
            normalEventCount.get(),
            normalLatestSequence.get(),
            slowLatestSequence.get(),
            slowFailed.get());
    }
}
