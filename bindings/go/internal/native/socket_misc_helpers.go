// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

func routingIDPointer(raw *C.zlink_routing_id_t) unsafe.Pointer {
	if raw == nil || raw.size == 0 {
		return nil
	}
	return unsafe.Pointer(&raw.data[0])
}

func getHandleRoutingID(handle unsafe.Pointer) (RoutingID, error) {
	var raw C.zlink_routing_id_t
	rc51, errno51 := C.zlink_get_routing_id(handle, &raw)
	if err := configErrorFromCall(rc51, errno51); err != nil {
		return RoutingID{}, err
	}
	return routingIDFromC(raw), nil
}

func withCStringPair(left string, right string, fn func(*C.char, *C.char) error) error {
	for _, value := range []string{left, right} {
		if err := validateEndpointString(value); err != nil {
			return err
		}
	}
	leftC := C.CString(left)
	defer C.free(unsafe.Pointer(leftC))
	rightC := C.CString(right)
	defer C.free(unsafe.Pointer(rightC))
	return fn(leftC, rightC)
}

func subscriptionAt(handle unsafe.Pointer, index int) (string, bool, error) {
	if index < 0 {
		return "", false, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	var size C.size_t
	var isPattern C.int
	rc52, errno52 := C.zlink_subscription_at(handle, C.size_t(index), nil, &size, &isPattern)
	err := configErrorFromCall(rc52, errno52)
	if err == nil {
		return "", isPattern != 0, nil
	}
	zerr, ok := err.(*ConfigError)
	if !ok || (zerr.Result != ConfigInvalidArgument && zerr.Result != ConfigBufferTooSmall) || size == 0 {
		return "", false, err
	}
	buf := make([]byte, int(size))
	rc53, errno53 := C.zlink_subscription_at(handle, C.size_t(index), (*C.char)(unsafe.Pointer(&buf[0])), &size, &isPattern)
	if err := configErrorFromCall(rc53, errno53); err != nil {
		return "", false, err
	}
	return string(buf[:int(size)]), isPattern != 0, nil
}

func setTLSServer(handle unsafe.Pointer, certPath string, keyPath string, requireClientCert bool) error {
	return withCStringPair(certPath, keyPath, func(certC *C.char, keyC *C.char) error {
		var required C.int
		if requireClientCert {
			required = 1
		}
		rc54, errno54 := C.zlink_set_tls_server(handle, certC, keyC, required)
		return configErrorFromCall(rc54, errno54)
	})
}

func setTLSClient(handle unsafe.Pointer, caCertPath string, hostname string, trustSystem bool) error {
	return withCStringPair(caCertPath, hostname, func(caCertC *C.char, hostnameC *C.char) error {
		var trust C.int
		if trustSystem {
			trust = 1
		}
		rc55, errno55 := C.zlink_set_tls_client(handle, caCertC, hostnameC, trust)
		return configErrorFromCall(rc55, errno55)
	})
}
