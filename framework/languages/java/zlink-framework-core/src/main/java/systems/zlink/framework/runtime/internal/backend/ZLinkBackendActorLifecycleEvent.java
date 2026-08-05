package systems.zlink.framework.runtime.internal.backend;

public record ZLinkBackendActorLifecycleEvent(
    ZLinkBackendActorLifecycleEventKind kind,
    ZLinkBackendActorLifecycleInfo info) {
}
