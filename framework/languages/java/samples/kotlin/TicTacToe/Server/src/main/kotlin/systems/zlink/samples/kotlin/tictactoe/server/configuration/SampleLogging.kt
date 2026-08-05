package systems.zlink.samples.kotlin.tictactoe.server.configuration

import java.nio.file.Files
import java.nio.file.Path

object SampleLogging {
    fun configure(settings: SampleSettings, role: String) {
        Files.createDirectories(Path.of(settings.logDirectory))
        Path.of(settings.logDirectory, "$role.log").toFile().createNewFile()
        Files.createDirectories(Path.of(settings.logDirectory))
    }

    fun flowLogPath(settings: SampleSettings, role: String): String =
        Path.of(settings.logDirectory, "flow-$role.log").toString()
}
