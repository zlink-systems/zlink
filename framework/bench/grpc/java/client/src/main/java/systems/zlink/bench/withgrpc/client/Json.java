/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.List;
import java.util.Map;

/** Minimal JSON writer for the cell records. The harness emits data, not prose (FB-021). */
final class Json {
    private Json() {
    }

    static void write(StringBuilder out, Object value, int depth) {
        String pad = "  ".repeat(depth);
        String innerPad = "  ".repeat(depth + 1);
        if (value instanceof Map<?, ?> map) {
            out.append("{\n");
            int index = 0;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                out.append(innerPad).append(quote(String.valueOf(entry.getKey()))).append(": ");
                write(out, entry.getValue(), depth + 1);
                out.append(++index < map.size() ? ",\n" : "\n");
            }
            out.append(pad).append('}');
            return;
        }
        if (value instanceof List<?> list) {
            out.append('[');
            for (int i = 0; i < list.size(); i++) {
                if (i > 0) {
                    out.append(", ");
                }
                write(out, list.get(i), depth + 1);
            }
            out.append(']');
            return;
        }
        if (value == null) {
            out.append("null");
            return;
        }
        if (value instanceof Number || value instanceof Boolean) {
            out.append(value);
            return;
        }
        out.append(quote(String.valueOf(value)));
    }

    private static String quote(String text) {
        StringBuilder out = new StringBuilder("\"");
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"' -> out.append("\\\"");
                case '\\' -> out.append("\\\\");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                default -> {
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
                }
            }
        }
        return out.append('"').toString();
    }
}
