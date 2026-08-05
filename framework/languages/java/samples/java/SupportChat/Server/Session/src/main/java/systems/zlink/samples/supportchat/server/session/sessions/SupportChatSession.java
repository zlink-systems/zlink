package systems.zlink.samples.supportchat.server.session.sessions;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.samples.supportchat.server.configuration.SampleNames;
import systems.zlink.samples.supportchat.server.configuration.SampleTimings;
import systems.zlink.samples.supportchat.shared.contracts.Messages;

public final class SupportChatSession implements ZLinkSession {
    private final ZLinkSessionContext context;
    private final ZLinkClient channels;
    private final Map<String, ZLinkSessionActor> conversationActors = new LinkedHashMap<>();
    private ZLinkSessionActor identityActor;
    private String identityActorId = "";
    private String identityDisplayName = "";
    private String identityRole = "";

    public SupportChatSession(ZLinkSessionContext context, ZLinkClient channels) {
        this.context = context;
        this.channels = channels;
    }

    @Override
    public ZLinkSessionContext context() {
        return context;
    }

    @Override
    public CompletionStage<Void> onConnected() {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnected() {
        CompletableFuture<?>[] notifications = context.actors().bound().stream()
            .map(actor -> actor.notifyDisconnected().toCompletableFuture())
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(notifications);
    }

    @Override
    public CompletionStage<Void> onError(ZLinkStreamError error) {
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDispatch(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return switch (dispatch.packetName()) {
            case "AuthenticateReq" -> authenticate(payload.decode(Messages.AuthenticateReq.class));
            case "JoinConversationReq" -> joinConversation(dispatch, payload);
            default -> relayConversationPacket(dispatch, payload);
        };
    }

    private CompletionStage<Void> authenticate(Messages.AuthenticateReq request) {
        return channels
            .requestToChannel(SampleNames.ApiChannel, new Messages.AuthenticateUserReq(request.accessToken()))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.AuthenticateUserRes.class)
            .thenCompose(authenticated -> {
                if (!authenticated.accepted()
                    || authenticated.actorId() == null
                    || authenticated.displayName() == null
                    || authenticated.role() == null) {
                    throw new IllegalStateException(
                        authenticated.reason() == null ? "SupportChat authentication failed" : authenticated.reason());
                }
                identityActorId = authenticated.actorId();
                identityDisplayName = authenticated.displayName();
                identityRole = authenticated.role();
                return channels.requestToChannel(
                        SampleNames.SupportChannel,
                        new Messages.EnsureSupportUserActorReq(
                            identityActorId,
                            identityDisplayName,
                            identityRole,
                            identityActorId))
                    .timeout(SampleTimings.RequestTimeout)
                    .submit(Messages.EnsureSupportUserActorRes.class);
            })
            .thenCompose(ensured -> bindOrGet(ensured.actor().toActorRef()))
            .thenAccept(actor -> {
                identityActor = actor;
                context.client()
                    .reply(new Messages.AuthenticateRes(
                        identityActorId, identityDisplayName, identityRole))
                    .submit();
            });
    }

    private CompletionStage<Void> joinConversation(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        if (SampleNames.Roles.Customer.equals(identityRole)) {
            return requireIdentityActor().relay(dispatch, payload).thenApply(ignored -> null);
        }
        String conversationId = requireConversationId(dispatch);
        ZLinkSessionActor existing = conversationActors.get(conversationId);
        if (existing != null) {
            return existing.relay(dispatch, payload).thenApply(ignored -> null);
        }
        return channels.requestToChannel(
                SampleNames.SupportChannel,
                new Messages.EnsureAgentConversationReq(
                    identityActorId, identityDisplayName, conversationId))
            .timeout(SampleTimings.RequestTimeout)
            .submit(Messages.EnsureAgentConversationRes.class)
            .thenCompose(ensured -> bindOrGet(ensured.actor().toActorRef())
                .thenAccept(actor -> {
                    conversationActors.put(conversationId, actor);
                    context.client().reply(
                        new Messages.JoinConversationRes(ensured.scheduled(), ensured.state())).submit();
                }));
    }

    private CompletionStage<Void> relayConversationPacket(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        String conversationId = dispatch.metadata().get(SampleNames.ConversationIdMetadataKey);
        ZLinkSessionActor target = conversationId == null
            ? null : conversationActors.get(conversationId);
        if (target == null) {
            target = requireIdentityActor();
        }
        return target.relay(dispatch, payload).thenApply(ignored -> null);
    }

    private CompletionStage<ZLinkSessionActor> bindOrGet(
        systems.zlink.framework.actors.ActorRef actorRef) {
        ZLinkSessionActor existing = context.actors().find(actorRef.actorId()).orElse(null);
        return existing == null
            ? context.actors().bind(actorRef)
            : CompletableFuture.completedFuture(existing);
    }

    private ZLinkSessionActor requireIdentityActor() {
        if (identityActor == null) {
            throw new IllegalStateException("Client must authenticate before sending conversation packets");
        }
        return identityActor;
    }

    private static String requireConversationId(ZLinkSessionDispatchContext dispatch) {
        String conversationId = dispatch.metadata().get(SampleNames.ConversationIdMetadataKey);
        if (conversationId == null || conversationId.isBlank()) {
            throw new IllegalStateException("Conversation packet is missing ConversationId metadata");
        }
        return conversationId;
    }
}
