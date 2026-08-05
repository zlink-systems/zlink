package systems.zlink.samples.supportchat.server.support.spots.conversationspot;

public record ConversationCreateReq(
    String customerActorId,
    String customerDisplayName,
    String subject,
    long createdAtUnixMs) {
}
