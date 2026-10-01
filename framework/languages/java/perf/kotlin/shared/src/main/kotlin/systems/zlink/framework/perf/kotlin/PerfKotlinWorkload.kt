package systems.zlink.framework.perf.kotlin

public fun planStreamTargets(targets: List<String>, streamCount: Int): List<String> {
    require(targets.isNotEmpty()) { "A workload requires at least one target." }
    return List(streamCount) { stream -> targets[stream % targets.size] }
}
