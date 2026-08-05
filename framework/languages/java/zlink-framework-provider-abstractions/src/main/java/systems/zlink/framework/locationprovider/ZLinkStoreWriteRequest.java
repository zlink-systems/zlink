package systems.zlink.framework.locationprovider;

import java.util.List;
import java.util.Objects;

public record ZLinkStoreWriteRequest(
    List<ZLinkStoreCondition> conditions,
    List<ZLinkStoreMutation> mutations) {
    public ZLinkStoreWriteRequest {
        conditions = List.copyOf(
            Objects.requireNonNull(conditions, "conditions"));
        mutations = List.copyOf(
            Objects.requireNonNull(mutations, "mutations"));
    }
}
