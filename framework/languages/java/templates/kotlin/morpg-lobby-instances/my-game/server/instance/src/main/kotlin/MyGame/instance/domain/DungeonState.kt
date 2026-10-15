package MyGame.instance.domain

data class DungeonState(
    var partyId: String = "",
    var members: List<String> = emptyList(),
    var stage: Int = 0,
)
