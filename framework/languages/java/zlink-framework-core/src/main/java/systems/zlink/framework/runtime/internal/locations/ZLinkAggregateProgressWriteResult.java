package systems.zlink.framework.runtime.internal.locations;

public sealed interface ZLinkAggregateProgressWriteResult
    permits ZLinkAggregateProgressStored, ZLinkAggregateProgressConflict {
}
