package systems.zlink.e2e.registrationcodec.main.Infrastructure;

import java.util.concurrent.atomic.AtomicInteger;

public final class DiSingletonDependency {
    private static final AtomicInteger NEXT_ID = new AtomicInteger();
    private final int id = NEXT_ID.incrementAndGet();

    public int id() {
        return id;
    }
}
