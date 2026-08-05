package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkBackendConnectableSocket extends ZLinkBackendSocket {
    void connect(String endpoint);

    void disconnect(String endpoint);
}
