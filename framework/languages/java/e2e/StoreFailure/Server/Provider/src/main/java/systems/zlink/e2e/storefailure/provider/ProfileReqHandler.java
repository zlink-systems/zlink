package systems.zlink.e2e.storefailure.provider;

import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ProfileReqHandler
    implements ZLinkRequestHandler<Contracts.ProfileReq, Contracts.ProfileRes> {
    private final ProviderEvidenceStore evidence;

    public ProfileReqHandler(ProviderEvidenceStore evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.ProfileRes> handle(
        Contracts.ProfileReq request,
        ZLinkMessageContext context) {
        evidence.record(request.marker(), request.value());
        return CompletableFuture.completedFuture(new Contracts.ProfileRes(
            "profile:" + request.value(),
            evidence.rid(),
            request.marker()));
    }
}
