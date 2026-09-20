package systems.zlink.tutorial.server.actors

import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorFactory

// --8<-- [start:actor-class]
// A player is addressed by its own id and carries state that outlives any one
// connection. Like a room, its messages run one at a time.
class Player(private val actorContext: ZLinkActorContext) : ZLinkActor {

    var nickname: String = "anonymous"
        private set

    override fun context(): ZLinkActorContext = actorContext

    fun rename(value: String) {
        nickname = value
    }
}

// --8<-- [end:actor-class]

// --8<-- [start:actor-factory]
// The Framework creates players through this factory rather than by calling a
// constructor, so dependencies can be injected here.
class PlayerFactory : ZLinkActorFactory {
    override fun create(context: ZLinkActorContext): CompletionStage<ZLinkActor> =
        CompletableFuture.completedFuture(Player(context))
}
// --8<-- [end:actor-factory]
