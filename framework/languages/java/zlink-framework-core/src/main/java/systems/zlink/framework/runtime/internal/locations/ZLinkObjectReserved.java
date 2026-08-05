package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkObjectReserved(ZLinkObjectReservation reservation)
    implements ZLinkObjectReserveResult {
    public ZLinkObjectReserved {
        Objects.requireNonNull(reservation, "reservation");
    }
}
