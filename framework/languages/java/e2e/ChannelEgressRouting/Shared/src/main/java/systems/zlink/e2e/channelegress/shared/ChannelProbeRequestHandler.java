package systems.zlink.e2e.channelegress.shared;

import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;

@ZLinkHandlerGroup(Contracts.HANDLER_GROUP)
public final class ChannelProbeRequestHandler
    implements ZLinkRequestHandler<Contracts.ChannelProbeReq, Contracts.ChannelProbeRes> {
    private final EvidenceState evidence;
    private final ZLinkRouteClient routes;
    private final ZLinkClient channels;

    public ChannelProbeRequestHandler(
        EvidenceState evidence,
        ZLinkRouteClient routes,
        ZLinkClient channels) {
        this.evidence = evidence;
        this.routes = routes;
        this.channels = channels;
        evidence.add("handler-created", "ChannelProbeRequestHandler");
    }

    @Override
    public CompletionStage<Contracts.ChannelProbeRes> handle(
        Contracts.ChannelProbeReq request,
        ZLinkMessageContext context) {
        String channel = context.channelName().orElse("<none>");
        evidence.add("request-start", "channel=" + channel + "|id=" + request.id());
        CompletionStage<Void> release = evidence.isHeld() && "hold".equals(request.mode())
            ? evidence.awaitReleaseAsync()
            : CompletableFuture.completedFuture(null);
        if ("play".equals(evidence.role())
            && Contracts.PLAY_CHANNEL.equals(channel)
            && "cascade".equals(request.mode())) {
            CompletionStage<Contracts.ChannelProbeRes> audit = routes
                .requestToChannel(
                    Contracts.AUDIT_CHANNEL,
                    new Contracts.ChannelProbeReq(request.id() + "-audit"))
                .timeout(java.time.Duration.ofSeconds(5))
                .submit(Contracts.ChannelProbeRes.class);
            CompletionStage<Contracts.ChannelProbeRes> workflow = channels
                .requestToChannel(
                    Contracts.WORKFLOW_CHANNEL,
                    new Contracts.ChannelProbeReq(request.id() + "-workflow"))
                .timeout(java.time.Duration.ofSeconds(5))
                .submit(Contracts.ChannelProbeRes.class);
            return release
                .thenCompose(ignored -> audit.thenCombine(
                    workflow,
                    (auditReply, workflowReply) -> new ArrayList<>(java.util.List.of(
                        auditReply.role() + ":" + auditReply.channel(),
                        workflowReply.role() + ":" + workflowReply.channel()))))
                .thenApply(downstream -> response(request, channel, downstream));
        }
        return release.thenApply(ignored -> response(request, channel, new ArrayList<>()));
    }

    private Contracts.ChannelProbeRes response(
        Contracts.ChannelProbeReq request,
        String channel,
        ArrayList<String> downstream) {
        evidence.add("request-end", "channel=" + channel + "|id=" + request.id());
        return new Contracts.ChannelProbeRes(
            request.id(),
            evidence.role(),
            evidence.rid(),
            channel,
            downstream);
    }
}
