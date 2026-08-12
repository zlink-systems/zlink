package systems.zlink.framework.runtime.internal.locations;

/** The aggregate commit CAS won before a retained abort could be claimed. */
public final class ZLinkAggregateAbortCommitWonException
    extends IllegalStateException {
    public ZLinkAggregateAbortCommitWonException() {
        super("aggregate commit won before retained abort");
    }
}
