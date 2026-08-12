package systems.zlink.samples.kotlin.zoneworld.shared

import java.nio.charset.StandardCharsets
import kotlin.math.abs

object ZoneWorldSpec {
    const val WORLD_SIZE = 100
    const val ZONE_SPLIT = 50
    const val BORDER_BAND = 10
    const val MAX_STEP_PER_AXIS = 5
    const val SPAWN_X = 25
    const val SPAWN_Y = 25
    const val TICK_PERIOD_MS = 100L
    const val BOT_TICK_PERIOD_MS = 500L
    const val BOT_STEP = 3
    const val BORDER_EXPIRY_TICKS = 3

    data class MoveDecision(val accepted: Boolean, val zoneChanged: Boolean, val reason: String? = null)
    data class BotFixture(val id: String, val x: Int, val y: Int, val dirX: Int, val dirY: Int)

    fun zoneOf(x: Int, y: Int): String = when {
        x < ZONE_SPLIT && y < ZONE_SPLIT -> "zone-nw"
        x >= ZONE_SPLIT && y < ZONE_SPLIT -> "zone-ne"
        x < ZONE_SPLIT -> "zone-sw"
        else -> "zone-se"
    }

    fun inRange(x: Int, y: Int) = x in 0 until WORLD_SIZE && y in 0 until WORLD_SIZE
    fun isWest(zone: String) = zone == "zone-nw" || zone == "zone-sw"
    fun isNorth(zone: String) = zone == "zone-nw" || zone == "zone-ne"

    fun adjacentZones(zone: String): List<String> = when (zone) {
        "zone-nw" -> listOf("zone-ne", "zone-sw")
        "zone-ne" -> listOf("zone-nw", "zone-se")
        "zone-sw" -> listOf("zone-nw", "zone-se")
        "zone-se" -> listOf("zone-ne", "zone-sw")
        else -> error("unknown zone: $zone")
    }

    fun nodeOf(zone: String) = when (zone) {
        "zone-nw", "zone-sw" -> "zone-node-1"
        "zone-ne", "zone-se" -> "zone-node-2"
        else -> error("unknown zone: $zone")
    }

    fun zonesOf(nodeId: String) = when (nodeId) {
        "zone-node-1" -> listOf("zone-nw", "zone-sw")
        "zone-node-2" -> listOf("zone-ne", "zone-se")
        else -> emptyList()
    }

    fun inBorderBand(x: Int, y: Int, from: String, to: String): Boolean {
        if (zoneOf(x, y) != from) return false
        val crossesX = isWest(from) != isWest(to)
        val crossesY = isNorth(from) != isNorth(to)
        if (crossesX == crossesY) return false
        val distance = if (crossesX) {
            if (isWest(from)) ZONE_SPLIT - 1 - x else x - ZONE_SPLIT
        } else {
            if (isNorth(from)) ZONE_SPLIT - 1 - y else y - ZONE_SPLIT
        }
        return abs(distance) < BORDER_BAND
    }

    fun validateMove(fromX: Int, fromY: Int, toX: Int, toY: Int): MoveDecision {
        if (!inRange(toX, toY)) return MoveDecision(false, false, "OutOfRange")
        if (abs(toX - fromX) > MAX_STEP_PER_AXIS || abs(toY - fromY) > MAX_STEP_PER_AXIS) {
            return MoveDecision(false, false, "TooFar")
        }
        val from = zoneOf(fromX, fromY)
        val to = zoneOf(toX, toY)
        if ((isWest(from) != isWest(to)) && (isNorth(from) != isNorth(to))) {
            return MoveDecision(false, false, "DiagonalCrossing")
        }
        return MoveDecision(true, from != to)
    }

    val utf8Order = Comparator<String> { left, right ->
        val a = left.toByteArray(StandardCharsets.UTF_8)
        val b = right.toByteArray(StandardCharsets.UTF_8)
        for (index in 0 until minOf(a.size, b.size)) {
            val difference = (a[index].toInt() and 0xff) - (b[index].toInt() and 0xff)
            if (difference != 0) return@Comparator difference
        }
        a.size - b.size
    }

    fun bots() = listOf(
        BotFixture("bot-nw-x", 10, 15, 1, 0), BotFixture("bot-nw-y", 15, 10, 0, 1),
        BotFixture("bot-ne-x", 90, 15, -1, 0), BotFixture("bot-ne-y", 85, 10, 0, 1),
        BotFixture("bot-sw-x", 10, 85, 1, 0), BotFixture("bot-sw-y", 15, 90, 0, -1),
        BotFixture("bot-se-x", 90, 85, -1, 0), BotFixture("bot-se-y", 85, 90, 0, -1),
    )
}
