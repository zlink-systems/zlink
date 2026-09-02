// SPDX-License-Identifier: MPL-2.0

package native

// Recv pulls ordinary DEALER DATA into reusable Received storage. Request
// replies arrive through the socket-local completion queue, not this path.
func (s *DealerSocket) Recv(out *Received, flags RecvFlags) (bool, error) {
	return (&directSocket{connectionSocket: s.connectionSocket}).Recv(out, flags)
}
