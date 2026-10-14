package MyGame

import java.time.Duration

object Control {
    const val DRAIN = "drain"
    const val SHUTDOWN = "shutdown"
    const val READY = "READY "
    const val RELOCATION_REQUESTED = "RELOCATION_REQUESTED"
    const val RELOCATED = "RELOCATED"
    const val SAFE_TO_SHUTDOWN = "SAFE_TO_SHUTDOWN"
    const val STOPPED = "STOPPED"
    const val PROCESS_EXIT = "PROCESS_EXIT "
    val HOST_DEADLINE: Duration = Duration.ofSeconds(30)
    const val PROCESS_SECONDS = 35L
}
