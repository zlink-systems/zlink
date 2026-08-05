package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkBackendObject extends AutoCloseable {
    String name();

    @Override
    void close();
}
