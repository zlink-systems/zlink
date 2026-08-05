package systems.zlink.framework.runtime.internal.spots;

import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;
import systems.zlink.framework.errors.ZLinkConfigurationException;

/**
 * Validates the Framework SpotId text contract without changing the supplied
 * identity. In particular, validation does not normalize Unicode or fold case.
 */
public final class ZLinkSpotIdValidator {
    private static final Pattern FRAMEWORK_ENTRY_SPOT_ID = Pattern.compile(
        ".+-entry-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}"
            + "-[89ab][0-9a-f]{3}-[0-9a-f]{12}");

    private ZLinkSpotIdValidator() {
    }

    public static String requireValid(String spotId) {
        if (spotId == null || spotId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                "SpotId must contain 1..255 UTF-8 bytes");
        }
        int byteLength = spotId.getBytes(StandardCharsets.UTF_8).length;
        if (byteLength == 0 || byteLength > 0xff) {
            throw new IllegalArgumentException(
                "SpotId must contain 1..255 UTF-8 bytes");
        }
        return spotId;
    }

    public static String requireCallerAssignable(String spotId) {
        String valid = requireValid(spotId);
        if (FRAMEWORK_ENTRY_SPOT_ID.matcher(valid).matches()) {
            throw new ZLinkConfigurationException(
                "SpotId uses the Framework-reserved Entry Spot format");
        }
        return valid;
    }
}
