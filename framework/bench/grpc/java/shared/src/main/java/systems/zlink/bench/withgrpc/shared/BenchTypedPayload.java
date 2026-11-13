/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

/** Typed JSON payload used by the L4 ladder path. */
public record BenchTypedPayload(byte[] body) {}
