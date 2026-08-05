package systems.zlink.e2e.kotlin.registrationcodec.main.infrastructure

import java.util.concurrent.atomic.AtomicInteger

class DiScopedDependency(
    private val state: ScenarioState,
) : AutoCloseable {
    val id: Int = nextId.incrementAndGet()
    private var closed = false

    override fun close() {
        if (!closed) {
            closed = true
            state.incrementDiDisposeCount()
        }
    }

    private companion object {
        val nextId = AtomicInteger()
    }
}

class DiSingletonDependency {
    val id: Int = nextId.incrementAndGet()

    private companion object {
        val nextId = AtomicInteger()
    }
}
