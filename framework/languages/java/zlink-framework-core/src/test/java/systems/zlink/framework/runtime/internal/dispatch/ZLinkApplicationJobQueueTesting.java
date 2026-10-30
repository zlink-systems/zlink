package systems.zlink.framework.runtime.internal.dispatch;

import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;

import java.util.OptionalLong;
import java.util.function.LongSupplier;

/** Test-only access to the existing package-private clock constructor. */
public final class ZLinkApplicationJobQueueTesting {
    private ZLinkApplicationJobQueueTesting() {}

    public static ZLinkApplicationJobQueue withClock(
            ZLinkApplicationJobQueueProfile profile,
            OptionalLong manualMax,
            ZLinkApplicationJobQueue.ProcessorCandidates processors,
            LongSupplier clock) {
        return new ZLinkApplicationJobQueue(profile, manualMax, processors, clock);
    }
}
