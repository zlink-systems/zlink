package systems.zlink.framework.runtime.internal.locations;

/** Limits shared by Framework Location Store repositories and the in-memory provider. */
public final class ZLinkLocationStoreLimits {
    /** Maximum unique keys referenced by one conditional Location Store batch. */
    public static final int MAXIMUM_BATCH_KEYS = 2048;

    private ZLinkLocationStoreLimits() {}
}
