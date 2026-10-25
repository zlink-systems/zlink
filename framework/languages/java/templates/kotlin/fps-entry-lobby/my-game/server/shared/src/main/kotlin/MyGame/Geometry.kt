package MyGame

import com.fasterxml.jackson.annotation.JsonIgnore
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

data class Vec3(
    val x: Double = 0.0,
    val y: Double = 0.0,
    val z: Double = 0.0,
) {
    @get:JsonIgnore
    val isFinite: Boolean
        get() = x.isFinite() && y.isFinite() && z.isFinite()

    @get:JsonIgnore
    val length: Double
        get() {
            val scale = max(abs(x), max(abs(y), abs(z)))
            if (scale == 0.0 || !scale.isFinite()) return scale
            val sx = x / scale
            val sy = y / scale
            val sz = z / scale
            return scale * sqrt(sx * sx + sy * sy + sz * sz)
        }

    operator fun plus(other: Vec3) = Vec3(x + other.x, y + other.y, z + other.z)

    operator fun minus(other: Vec3) = Vec3(x - other.x, y - other.y, z - other.z)

    operator fun times(scale: Double) = Vec3(x * scale, y * scale, z * scale)

    fun normalizedOrNull(): Vec3? {
        val scale = max(abs(x), max(abs(y), abs(z)))
        if (!isFinite || scale == 0.0) return null
        val scaled = Vec3(x / scale, y / scale, z / scale)
        return scaled * (1.0 / scaled.length)
    }
}

data class Aabb(val min: Vec3, val max: Vec3) {
    fun contains(point: Vec3) =
        point.x >= min.x && point.x <= max.x &&
            point.y >= min.y && point.y <= max.y &&
            point.z >= min.z && point.z <= max.z

    // A closed slab interval includes faces, edges, corners, and origins inside the box.
    fun intersect(origin: Vec3, direction: Vec3, limit: Double): Double? {
        var near = 0.0
        var far = limit

        fun slab(originValue: Double, directionValue: Double, minimum: Double, maximum: Double): Boolean {
            if (directionValue == 0.0) return originValue >= minimum && originValue <= maximum
            val a = (minimum - originValue) / directionValue
            val b = (maximum - originValue) / directionValue
            near = max(near, min(a, b))
            far = min(far, max(a, b))
            return near <= far
        }

        if (!slab(origin.x, direction.x, min.x, max.x) ||
            !slab(origin.y, direction.y, min.y, max.y) ||
            !slab(origin.z, direction.z, min.z, max.z)
        ) return null
        return near
    }
}

object ArenaMap {
    val bounds = Aabb(Vec3(0.0, 0.0, 0.0), Vec3(100.0, 10.0, 100.0))
    val walls = listOf(
        Aabb(Vec3(49.0, 0.0, 20.0), Vec3(51.0, 10.0, 60.0)),
        Aabb(Vec3(80.0, 0.0, 0.0), Vec3(82.0, 10.0, 20.0)),
    )
    val spawns = listOf(Vec3(48.0, 0.0, 18.0), Vec3(52.0, 0.0, 18.0))

    fun playerBox(feet: Vec3) = Aabb(
        feet + Vec3(-Game.PLAYER_HALF_WIDTH, 0.0, -Game.PLAYER_HALF_WIDTH),
        feet + Vec3(Game.PLAYER_HALF_WIDTH, Game.PLAYER_HEIGHT, Game.PLAYER_HALF_WIDTH),
    )
}
