package systems.zlink.framework.locationprovider;

@FunctionalInterface
public interface ZLinkStoreCancellation {
    boolean isCancellationRequested();
}
