// SPDX-License-Identifier: MPL-2.0

package native

func monitorHasRoutingID(routingID RoutingID) bool {
	return routingID.Size() > 0
}
