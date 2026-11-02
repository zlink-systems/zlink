package MyGame.e2e

import java.nio.file.Path
import java.util.UUID
import java.util.concurrent.TimeUnit

internal object DockerRuntime {
    private const val PROJECT_ENV = "MY_GAME_DOCKER_PROJECT"
    private const val IMAGE_ENV = "MY_GAME_NODE_IMAGE"
    private const val RUNNER_ENV = "MY_GAME_RUNNER_CONTAINER"
    private const val LABEL = "my-game.e2e"
    private const val CONFIG = "/template/node.json"
    val enabled = System.getenv(PROJECT_ENV) != null

    private fun execute(vararg arguments: String) {
        val process = ProcessBuilder(listOf("docker") + arguments).redirectErrorStream(true).start()
        val output = process.inputReader().use { it.readText() }
        check(process.waitFor() == 0) { "Docker ${arguments.first()} failed: $output" }
    }

    // SIGQUIT makes HotSpot print a thread dump to stdout; failures here must not hide the real error.
    fun threadDump(process: Process) {
        try {
            if (enabled) {
                // The container name is the last argument of `docker start -a -i <container>`.
                val container = process.info().arguments().get().last()
                execute("kill", "--signal=QUIT", container)
            } else if (!System.getProperty("os.name").startsWith("Windows")) {
                ProcessBuilder("kill", "-3", process.pid().toString())
                    .start()
                    .waitFor(3, TimeUnit.SECONDS)
            }
        } catch (_: Exception) {}
        Thread.sleep(3000)
    }

    fun process(command: List<String>, config: Path): ProcessBuilder {
        if (!enabled) return ProcessBuilder(command)
        val project = requireNotNull(System.getenv(PROJECT_ENV))
        val container = "$project-node-${UUID.randomUUID()}"
        execute(
            "create",
            "--rm",
            "-i",
            "--name",
            container,
            "--label",
            "$LABEL=$project",
            "--network",
            "container:${requireNotNull(System.getenv(RUNNER_ENV))}",
            requireNotNull(System.getenv(IMAGE_ENV)),
            *command.drop(1).dropLast(1).toTypedArray(),
            CONFIG,
        )
        // Copy through the Docker API; host and runner paths can differ on Windows.
        execute("cp", config.toString(), "$container:$CONFIG")
        return ProcessBuilder("docker", "start", "-a", "-i", container)
    }
}
