package systems.zlink.e2e.registrationcodec.main.Infrastructure;

import java.util.concurrent.atomic.AtomicInteger;

public final class DiScopedDependency implements AutoCloseable {
    private static final AtomicInteger NEXT_ID = new AtomicInteger();
    private final EvidenceStore state;
    private final int id = NEXT_ID.incrementAndGet();
    private boolean closed;

    public DiScopedDependency(EvidenceStore state) {
        this.state = state;
    }

    public int id() {
        return id;
    }

    @Override
    public void close() {
        if (!closed) {
            closed = true;
            state.incrementDiDisposeCount();
        }
    }
}
