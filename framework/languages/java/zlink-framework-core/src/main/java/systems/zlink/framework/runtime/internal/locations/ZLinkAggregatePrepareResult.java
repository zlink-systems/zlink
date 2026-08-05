package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkAggregatePrepareResult
    permits ZLinkAggregatePrepared, ZLinkAggregateAlreadyPrepared,
        ZLinkAggregateConflict, ZLinkAggregateStale,
        ZLinkAggregateGenerationExhausted {
}
