package Handlers;

import systems.zlink.e2e.registrationcodec.main.Handlers;
import java.util.Optional;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

final class FilterOrderValues {
    private FilterOrderValues() {
    }

    static Optional<String> from(Object request) {
        if (request instanceof Contracts.EchoManualReq manual
            && manual.value().startsWith("filter-order")) {
            return Optional.of(manual.value());
        }
        return Optional.empty();
    }
}
