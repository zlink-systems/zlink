package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkStreamBackendAdapter {
    ZLinkBackendStreamSocket createStreamSocket(
        ZLinkBackendContext context,
        ZLinkInternalMeshNode meshNode);
}
