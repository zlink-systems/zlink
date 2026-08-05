package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.util.List;

public record ZLinkLocationPage<T>(
    List<T> items,
    String continuationToken) {
}
