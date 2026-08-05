package systems.zlink.framework.locations;

import java.util.concurrent.CompletionStage;

public interface ZLinkLocationRuntimeQuery {
    CompletionStage<ZLinkLocationRuntimeStatus> getStatus();

    CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> listTopology(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page);

    CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page);
}
