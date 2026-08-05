package systems.zlink.framework.runtime.internal.locations;

/** The aggregate-marker progress CAS lost its StoreVersion race. */
public record ZLinkAggregateProgressConflict()
    implements ZLinkAggregateProgressWriteResult {
}
