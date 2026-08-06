package systems.zlink.framework.locations;

import java.util.concurrent.CompletionStage;
import java.util.Optional;

public interface ZLinkLocationRuntimeQuery {
    CompletionStage<ZLinkLocationRuntimeStatus> getStatus();

    CompletionStage<ZLinkLocationPage<ZLinkLocationTopologyEntry>> listTopology(
        ZLinkLocationTopologyFilter filter,
        ZLinkPageRequest page);

    CompletionStage<ZLinkLocationPage<ZLinkLocationServiceSummary>> listServiceSummaries(
        ZLinkLocationServiceSummaryFilter filter,
        ZLinkPageRequest page);

    CompletionStage<Optional<ZLinkLocationObjectEntry>> findActorLocation(String actorId);

    CompletionStage<Optional<ZLinkLocationObjectEntry>> findSpotLocation(String spotId);

    CompletionStage<ZLinkLocationPage<ZLinkLocationObjectEntry>> listObjectLocations(
        ZLinkLocationObjectFilter filter,
        ZLinkPageRequest page);
}
