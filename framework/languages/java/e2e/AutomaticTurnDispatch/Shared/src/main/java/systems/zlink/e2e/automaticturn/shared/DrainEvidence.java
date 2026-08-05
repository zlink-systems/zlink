package systems.zlink.e2e.automaticturn.shared;

import java.util.List;
import java.util.concurrent.Flow;
import java.util.concurrent.CopyOnWriteArrayList;
import systems.zlink.framework.monitoring.ZLinkFrameworkRuntimeStatus;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import java.time.Instant;

public final class DrainEvidence
    implements Flow.Subscriber<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> {
    public record Event(String state, Instant timestamp) {}

    private final List<Event> events = new CopyOnWriteArrayList<>();
    private Flow.Subscription subscription;

    @Override
    public void onSubscribe(Flow.Subscription subscription) {
        this.subscription = subscription;
        subscription.request(Long.MAX_VALUE);
    }

    @Override
    public void onNext(ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus> observed) {
        ZLinkFrameworkRuntimeStatus status = observed.status();
        events.add(new Event(status.state().name(), status.observedAt()));
    }

    @Override
    public void onError(Throwable error) {
        events.add(new Event("ERROR", Instant.now()));
    }

    @Override
    public void onComplete() {
        subscription = null;
    }

    public void observe(
        Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> statuses) {
        statuses.subscribe(this);
    }

    public List<Event> events() {
        return List.copyOf(events);
    }

    public void observeServing() {
        if (events.stream().noneMatch(event -> event.state().equals("SERVING"))) {
            events.add(new Event("SERVING", Instant.now()));
        }
    }
}
