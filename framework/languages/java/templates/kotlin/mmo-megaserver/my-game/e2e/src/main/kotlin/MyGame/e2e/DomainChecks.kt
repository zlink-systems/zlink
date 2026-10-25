package MyGame.e2e

import MyGame.*
import MyGame.region.domain.*
import com.fasterxml.jackson.core.type.TypeReference

fun checkDomain() {
    var count = 0
    fun verify(ok: Boolean, name: String) { check(ok) { "Domain boundary failed: $name" }; count++ }
    fun rejected(p: Positions, id: String, delta: Double, sequence: Long, reason: String, retry: Int): Boolean {
        val before = p.snapshot()
        return p.move(id, delta, sequence) == ActionRejected(reason, retry) && before == p.snapshot()
    }
    verify(Names.MAX_SPEED == Names.MAX_STEP * 5.0, "movement speed contract")
    var now = 0L
    val p = Positions { now }
    p.admit("p1", 2)
    verify(p.move("p1", Names.MAX_STEP.toDouble(), 1) == null && p.view() == listOf(PlayerPosition("p1", Names.MAX_STEP.toDouble(), 1)), "first movement accepts MaxStep")
    verify(rejected(p, "p1", Names.MAX_STEP.toDouble(), 2, "move-budget", 200), "budget rejection preserves state")
    val before = p.snapshot()
    val fractional = p.move("p1", 0.01, 2)
    verify(fractional?.reason == "move-budget" && fractional.retryAfterMs > 0 && before == p.snapshot(), "fractional budget retry")
    verify(rejected(p, "p1", Names.MAX_STEP + 1.0, 2, "step-too-large", 0), "step maximum plus one")
    verify(rejected(p, "p1", 1.0, 1, "stale-sequence", 0), "current sequence stale")
    for (value in listOf(Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY))
        verify(rejected(p, "p1", value, 2, "non-finite", 0), "non-finite domain input")
    val edge = Positions { now }
    edge.restore(mapOf("edge" to Position(Names.REGION_MAX_X.toDouble(), 0, 0)))
    verify(rejected(edge, "edge", 1.0, 1, "outside-region", 0), "maximum plus one")
    edge.restore(mapOf("edge" to Position(Names.REGION_MIN_X.toDouble(), 0, 0)))
    verify(rejected(edge, "edge", -1.0, 1, "outside-region", 0), "minimum minus one")
    val safe = Positions { now }
    safe.admit("safe", 2)
    verify(safe.move("safe", 0.0, Names.MAX_SAFE_INTEGER) == null, "maximum sequence")
    verify(rejected(safe, "safe", 0.0, Names.MAX_SAFE_INTEGER + 1, "integer-out-of-range", 0), "maximum sequence plus one")
    val oversized = Positions { now }
    oversized.admit("large", 2)
    verify(rejected(oversized, "large", Double.MAX_VALUE, 1, "step-too-large", 0), "finite oversized delta")
    val source = Positions { 0L }
    source.admit("relocated", 2)
    verify(source.move("relocated", Names.MAX_STEP.toDouble(), 1) == null, "snapshot source move")
    val snapshot = source.snapshot()
    verify(snapshot.getValue("relocated").remainingMs == 200L, "remaining milliseconds")
    verify(snapshot == source.snapshot(), "capture preserves source")
    var targetNow = 8L * 3600 * 1_000_000_000L
    val target = Positions { targetNow }
    target.restore(Json.mapper.readValue(Json.mapper.writeValueAsBytes(snapshot), object : TypeReference<LinkedHashMap<String, Position>>() {}))
    verify(source.view() == target.view() && target.snapshot().getValue("relocated").remainingMs == 200L, "different clock origin restore")
    verify(rejected(target, "relocated", Names.MAX_STEP.toDouble(), 2, "move-budget", 200), "restored budget retained")
    targetNow += 200 * 1_000_000L
    verify(target.move("relocated", Names.MAX_STEP.toDouble(), 2) == null, "server retry boundary")
    println("Domain boundaries: $count/20 passed")
}
