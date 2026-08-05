package systems.zlink.e2e.registrationcodec.main.Handlers;

import org.springframework.beans.factory.ObjectProvider;
import systems.zlink.e2e.registrationcodec.shared.Contracts;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.DiScopedDependency;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.DiSingletonDependency;
import systems.zlink.e2e.registrationcodec.main.Infrastructure.EvidenceStore;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;

public final class DiLifecycleReqHandler
    implements ZLinkRequestHandler<Contracts.DiLifecycleReq, Contracts.DiLifecycleRes> {
    private final ObjectProvider<DiScopedDependency> scoped;
    private final DiSingletonDependency singleton;
    private final EvidenceStore state;

    public DiLifecycleReqHandler(
        ObjectProvider<DiScopedDependency> scoped,
        DiSingletonDependency singleton,
        EvidenceStore state) {
        this.scoped = scoped;
        this.singleton = singleton;
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.DiLifecycleRes> handle(
        Contracts.DiLifecycleReq request,
        ZLinkMessageContext context) {
        int scopedId;
        try (DiScopedDependency dependency = scoped.getObject()) {
            scopedId = dependency.id();
            state.record(
                "DI",
                context.packetName(),
                scopedId + ":" + singleton.id() + ":" + request.value());
        }
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.DiLifecycleRes(
            "echo:" + request.value(),
            scopedId,
            singleton.id(),
            state.diDisposeCount()));
    }
}
