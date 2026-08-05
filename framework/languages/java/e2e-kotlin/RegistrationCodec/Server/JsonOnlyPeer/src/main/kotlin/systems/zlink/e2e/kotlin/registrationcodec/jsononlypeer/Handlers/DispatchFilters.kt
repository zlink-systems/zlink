package systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.handlers

import java.util.concurrent.CompletionStage
import systems.zlink.e2e.kotlin.registrationcodec.jsononlypeer.infrastructure.ScenarioState
import systems.zlink.framework.ZLinkHandlerFilter
import systems.zlink.framework.ZLinkHandlerFilterContext
import systems.zlink.framework.ZLinkHandlerFilterNext

class FirstOrderFilter(
    private val state: ScenarioState,
) : ZLinkHandlerFilter {
    override fun <T : Any?> invoke(
        context: ZLinkHandlerFilterContext,
        next: ZLinkHandlerFilterNext<T>,
    ): CompletionStage<T> {
        record(context, "first-before")
        return next.invoke().whenComplete { _, _ -> record(context, "first-after") }
    }

    private fun record(context: ZLinkHandlerFilterContext, step: String) {
        state.record("Filter", context.packetName(), step)
    }
}

class SecondOrderFilter(
    private val state: ScenarioState,
) : ZLinkHandlerFilter {
    override fun <T : Any?> invoke(
        context: ZLinkHandlerFilterContext,
        next: ZLinkHandlerFilterNext<T>,
    ): CompletionStage<T> {
        record(context, "second-before")
        return next.invoke().whenComplete { _, _ -> record(context, "second-after") }
    }

    private fun record(context: ZLinkHandlerFilterContext, step: String) {
        state.record("Filter", context.packetName(), step)
    }
}
