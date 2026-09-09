/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import java.util.List;

public final class Args {
    private Args() {
    }

    public static String value(String[] argv, String name, String fallback) {
        List<String> list = List.of(argv);
        int index = list.indexOf(name);
        return index >= 0 && index + 1 < list.size() ? list.get(index + 1) : fallback;
    }

    public static int integer(String[] argv, String name, int fallback) {
        String raw = value(argv, name, null);
        if (raw == null) {
            return fallback;
        }
        try {
            return Integer.parseInt(raw.trim());
        } catch (NumberFormatException error) {
            return fallback;
        }
    }

    public static double number(String[] argv, String name, double fallback) {
        String raw = value(argv, name, null);
        if (raw == null) {
            return fallback;
        }
        try {
            return Double.parseDouble(raw.trim());
        } catch (NumberFormatException error) {
            return fallback;
        }
    }
}
