/* SPDX-License-Identifier: Elastic-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.framework.runtime.internal.json.ZLinkFrameworkJsonProfile;

import java.io.IOException;

/** Reuses the framework-json-v1 mapper for the binding JSON row. */
public final class BenchGameJson {
    private static final ObjectMapper MAPPER = ZLinkFrameworkJsonProfile.mapper();

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
