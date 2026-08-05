/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.eventing;

/**
 * One ready source reported by a poller wait.
 * @param sourceKind whether the source is a socket, file descriptor, or timer
 * @param slot the caller token supplied when the source was registered
 * @param revents the returned poll event flags
 * @param fd the file descriptor, if the source is a file descriptor
 */
public record PollEvent(PollSourceKind sourceKind, long slot, int revents,
                        int fd) {
}
