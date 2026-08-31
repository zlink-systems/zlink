package systems.zlink.samples.kotlin.tictactoe.client

object TicTacToeClientArguments {
    fun parse(args: Array<String>): TicTacToeClientOptions {
        val defaults = TicTacToeClientOptions.createDefault()
        return TicTacToeClientOptions(
            apiUrl = readOption(args, "--api-url") ?: defaults.apiUrl,
            gameName = readOption(args, "--game-name") ?: defaults.gameName,
            xActorId = readOption(args, "--x-actor-id") ?: defaults.xActorId,
            oActorId = readOption(args, "--o-actor-id") ?: defaults.oActorId,
            observerActorId = readOption(args, "--observer-actor-id") ?: defaults.observerActorId,
            lifecycleCompletionFile =
                readOption(args, "--lifecycle-completion-file") ?: defaults.lifecycleCompletionFile,
        )
    }

    private fun readOption(args: Array<String>, name: String): String? {
        val index = args.indexOf(name)
        if (index < 0) {
            return null
        }
        require(index + 1 < args.size) { "Missing value for '$name'." }
        return args[index + 1]
    }
}
