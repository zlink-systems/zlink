package systems.zlink.e2e.registrymessaging.provider.Handlers;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class PayloadReqHandler
    implements ZLinkRequestHandler<Contracts.PayloadReq, Contracts.PayloadRes> {
    private final ScenarioState state;

    public PayloadReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public java.util.concurrent.CompletionStage<Contracts.PayloadRes> handle(
        Contracts.PayloadReq request,
        ZLinkMessageContext context) {
        state.record("payload-request", request.marker());
        return java.util.concurrent.CompletableFuture.completedFuture(new Contracts.PayloadRes(
            request.marker(),
            request.payload().length(),
            sha256(request.payload())));
    }

    private static String sha256(String payload) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            return HexFormat.of().formatHex(digest.digest(payload.getBytes(StandardCharsets.UTF_8))).toUpperCase();
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 unavailable", error);
        }
    }
}
