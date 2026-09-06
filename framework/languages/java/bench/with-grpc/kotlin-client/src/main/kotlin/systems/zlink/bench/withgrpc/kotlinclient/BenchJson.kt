/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.kotlinclient

/** Minimal JSON writer for the cell records. The harness emits data, not prose (FB-021). */
internal object BenchJson {
    fun write(out: StringBuilder, value: Any?, depth: Int) {
        val pad = "  ".repeat(depth)
        val innerPad = "  ".repeat(depth + 1)
        when (value) {
            is Map<*, *> -> {
                out.append("{\n")
                var index = 0
                for ((key, entry) in value) {
                    out.append(innerPad).append(quote(key.toString())).append(": ")
                    write(out, entry, depth + 1)
                    out.append(if (++index < value.size) ",\n" else "\n")
                }
                out.append(pad).append('}')
            }
            is List<*> -> {
                out.append('[')
                value.forEachIndexed { index, element ->
                    if (index > 0) {
                        out.append(", ")
                    }
                    write(out, element, depth + 1)
                }
                out.append(']')
            }
            null -> out.append("null")
            is Number, is Boolean -> out.append(value.toString())
            else -> out.append(quote(value.toString()))
        }
    }

    private fun quote(text: String): String {
        val out = StringBuilder("\"")
        for (character in text) {
            when (character) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else ->
                    if (character.code < 0x20) {
                        out.append(String.format("\\u%04x", character.code))
                    } else {
                        out.append(character)
                    }
            }
        }
        return out.append('"').toString()
    }
}
