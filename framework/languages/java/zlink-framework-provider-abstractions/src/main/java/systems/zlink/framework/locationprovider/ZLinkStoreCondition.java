package systems.zlink.framework.locationprovider;

import java.util.List;

public sealed interface ZLinkStoreCondition
        permits ZLinkStoreMissingCondition,
                ZLinkStoreVersionCondition,
                ZLinkStoreValueCondition,
                ZLinkStoreSumAtMostCondition {
    default List<ZLinkStoreKey> keys() {
        return switch (this) {
            case ZLinkStoreMissingCondition missing -> List.of(missing.key());
            case ZLinkStoreVersionCondition version -> List.of(version.key());
            case ZLinkStoreValueCondition value -> List.of(value.key());
            case ZLinkStoreSumAtMostCondition sum -> sum.keys();
        };
    }
}
