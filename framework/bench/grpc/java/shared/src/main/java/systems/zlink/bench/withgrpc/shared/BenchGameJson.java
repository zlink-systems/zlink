/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.fasterxml.jackson.databind.ObjectMapper;

import java.io.IOException;

/** Standard Jackson mapping used by the binding JSON row. */
public final class BenchGameJson {
    private static final ObjectMapper MAPPER = new ObjectMapper();

    private BenchGameJson() {}

    public static byte[] encode(BenchGamePayload payload) {
        try {
            return MAPPER.writeValueAsBytes(payload);
        } catch (IOException error) {
            throw new IllegalStateException("cannot encode game JSON payload", error);
        }
    }

    public static BenchGamePayload decode(byte[] json) {
        try {
            return MAPPER.readValue(json, BenchGamePayload.class);
        } catch (IOException error) {
            throw new IllegalArgumentException("cannot decode game JSON payload", error);
        }
    }
}
