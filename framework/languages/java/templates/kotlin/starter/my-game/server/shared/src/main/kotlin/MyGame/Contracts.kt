package MyGame

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game"
    const val STREAM = "players"
    const val SESSION = "session"
    val SCENARIOS = Scenario.entries.map { it.id }
}

object Roles {
    const val SESSION = "session"
    const val EXTERNAL = "external"
    const val LOCATION = "location-store"
}

enum class Scenario(val id: String) {
    CONNECT("connect");

    companion object {
        fun parse(id: String) =
            entries.firstOrNull { it.id == id }
                ?: throw IllegalArgumentException("Unknown scenario: $id")
    }
}
