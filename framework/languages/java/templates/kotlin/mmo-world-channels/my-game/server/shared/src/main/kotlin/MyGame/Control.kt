package MyGame

import java.time.Duration

object Control {
    const val CREATE_UPDATE_ZONE = "create-update-zone"
    const val UPDATE_ZONE_CREATED = "UPDATE_ZONE_CREATED"
    const val DRAIN = "drain"
    const val SHUTDOWN = "shutdown"
    const val READY = "READY "
    const val RELOCATION_REQUESTED = "RELOCATION_REQUESTED"
    const val RELOCATED = "RELOCATED"
    const val SAFE_TO_SHUTDOWN = "SAFE_TO_SHUTDOWN"
    const val STOPPED = "STOPPED"
    const val PROCESS_EXIT = "PROCESS_EXIT "
    const val PLAYER_CREATED = "PLAYER_CREATED "
    val HOST_DEADLINE: Duration = Duration.ofSeconds(30)
    val MESSAGE_FOLLOW: Duration = Duration.ofSeconds(30)
    val SAFE_DEADLINE: Duration = MESSAGE_FOLLOW.plus(HOST_DEADLINE)
    const val PROCESS_SECONDS = 35L
}
