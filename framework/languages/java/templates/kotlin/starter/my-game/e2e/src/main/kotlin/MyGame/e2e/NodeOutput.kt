package MyGame.e2e

import MyGame.Control
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.BlockingQueue
import kotlin.concurrent.thread

internal fun foregroundJavaPrefix(foreground: Boolean): List<String> =
    if (foreground && !isWindows()) listOf("sh", "-c", "trap '' INT; exec \"\$@\"", "node")
    else emptyList()

internal fun foregroundJavaFlags(foreground: Boolean): List<String> =
    if (foreground && isWindows()) listOf("-Xrs") else emptyList()

internal fun captureNodeOutput(
    id: String,
    process: Process,
    logs: Path,
    foreground: Boolean,
    control: BlockingQueue<String>,
): Thread =
    thread(name = "$id-output", isDaemon = true) {
        try {
            Files.newBufferedWriter(logs.resolve("$id-stdout.log")).use { stdout ->
                Files.newBufferedWriter(logs.resolve("$id-message-flow.log")).use { flow ->
                    process.inputReader(Charsets.UTF_8).useLines { lines ->
                        lines.forEach { line ->
                            if (line.startsWith("FLOW ")) {
                                flow.appendLine(line.removePrefix("FLOW "))
                                flow.flush()
                            } else {
                                stdout.appendLine(line)
                                stdout.flush()
                                when {
                                    line.startsWith("EVENT ") -> {
                                        if (foreground)
                                            println("[$id] ${line.removePrefix("EVENT ")}")
                                    }
                                    line.startsWith(Control.READY) -> {
                                        if (foreground) println("[$id] Ready")
                                        control.add(line)
                                    }
                                    line == Control.RELOCATION_REQUESTED ||
                                        line == Control.RELOCATED ||
                                        line == Control.SAFE_TO_SHUTDOWN ||
                                        line.startsWith(Control.STOPPED) -> control.add(line)
                                }
                            }
                        }
                    }
                }
            }
            control.add(Control.PROCESS_EXIT + process.waitFor())
        } catch (error: Exception) {
            control.add(Control.PROCESS_EXIT + error)
        }
    }
