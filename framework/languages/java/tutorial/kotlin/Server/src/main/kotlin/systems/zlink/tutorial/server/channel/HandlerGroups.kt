package systems.zlink.tutorial.server.channel

// A Kotlin suspending handler cannot be registered by type: the Java builder's
// addRequestHandler/addSendHandler require a class that implements the Java
// ZLinkRequestHandler/ZLinkSendHandler, which a suspend fun cannot override.
// Handler groups are the way in. Each handler class carries @ZLinkHandlerGroup,
// the channel registration names the same group, and only handlers in that group
// are exposed on that channel.
object HandlerGroups {
    const val PROFILE = "profile-handlers"
    const val TICKETING = "ticketing-handlers"
}
