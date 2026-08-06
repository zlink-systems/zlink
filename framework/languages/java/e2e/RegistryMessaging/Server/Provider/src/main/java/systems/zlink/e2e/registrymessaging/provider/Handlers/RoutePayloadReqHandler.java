package systems.zlink.e2e.registrymessaging.provider.Handlers;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrymessaging.provider.Infrastructure.ScenarioState;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;

public final class RoutePayloadReqHandler
    implements ZLinkRouteRequestHandler<Contracts.PayloadReq, Contracts.PayloadRes> {
    private final ScenarioState state;

    public RoutePayloadReqHandler(ScenarioState state) {
        this.state = state;
    }

    @Override
    public CompletionStage<Contracts.PayloadRes> handle(
        Contracts.PayloadReq request,
        ZLinkRouteMessageContext context) {
        state.record("payload-request", request.marker());
        return CompletableFuture.completedFuture(new Contracts.PayloadRes(
            request.marker(),
            request.payload().length(),
            sha256(request.payload())));
    }

    private static String sha256(String payload) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            return HexFormat.of().formatHex(
                digest.digest(payload.getBytes(StandardCharsets.UTF_8))).toUpperCase();
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 unavailable", error);
        }
    }
}
