package MyGame.channel.application

import java.util.concurrent.CompletionStage

interface PlayerRepository {
    fun load(id: String): CompletionStage<Long>

    fun save(id: String, gold: Long): CompletionStage<Void>
}
