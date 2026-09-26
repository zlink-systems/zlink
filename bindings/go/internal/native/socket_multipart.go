// SPDX-License-Identifier: MPL-2.0

package native

/*
#include <stdint.h>
#include <stdlib.h>
#include "zlink.h"
*/
import "C"

import "unsafe"

func closeNativeMultipart(parts []C.zlink_msg_t, count int) {
	if count <= 0 || len(parts) == 0 {
		return
	}
	C.zlink_multipart_close(&parts[0], C.size_t(count))
}

func closeMessageSlice(parts []*Message) {
	for _, part := range parts {
		if part != nil {
			_ = part.Close()
		}
	}
}

type preparedMultipart struct {
	native []C.zlink_msg_t
	parts  []*Message
}

type multipartSubmitFunc func(*C.zlink_msg_t, C.size_t) error
type multipartRecvFunc func(*C.zlink_msg_t, C.size_t, *C.size_t, C.zlink_recv_flags_t) (C.zlink_recv_result_t, error)

func (p *preparedMultipart) restore() error {
	if p == nil {
		return nil
	}
	for i, part := range p.parts {
		if part == nil {
			continue
		}
		rc56, errno56 := C.zlink_msg_move(&part.msg, &p.native[i])
		if err := configErrorFromCall(rc56, errno56); err != nil {
			closeNativeMultipart(p.native, len(p.native))
			return err
		}
	}
	closeNativeMultipart(p.native, len(p.native))
	return nil
}

func submitPreparedMultipart(prepared *preparedMultipart, submit multipartSubmitFunc) error {
	if prepared == nil || len(prepared.native) == 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	// Whole-message submission consumes every native slot on both success and
	// failure, so this prepared array has no ownership after the call returns.
	return submit(&prepared.native[0], C.size_t(len(prepared.native)))
}

func initNativeMessageFromBytes(native *C.zlink_msg_t, data []byte) error {
	if native == nil {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	rc57, errno57 := C.zlink_msg_init_size(native, C.size_t(len(data)))
	if err := configErrorFromCall(rc57, errno57); err != nil {
		return err
	}
	if len(data) > 0 {
		copy(unsafe.Slice((*byte)(C.zlink_msg_data(native)), len(data)), data)
	}
	return nil
}

func submitMultipartFromClones(parts []*Message, consumeOriginal bool, submit multipartSubmitFunc) error {
	if consumeOriginal && len(parts) == 1 {
		return submitSinglePartFromCopy(parts[0], submit)
	}
	if len(parts) == 0 {
		return configInvalidArgumentError()
	}
	native := make([]C.zlink_msg_t, len(parts))
	for i, part := range parts {
		if part == nil {
			closeNativeMultipart(native, i)
			return configInvalidArgumentError()
		}
		rc58, errno58 := C.zlink_msg_init(&native[i])
		if err := configErrorFromCall(rc58, errno58); err != nil {
			closeNativeMultipart(native, i)
			return err
		}
		rc59, errno59 := C.zlink_msg_copy(&native[i], &part.msg)
		if err := configErrorFromCall(rc59, errno59); err != nil {
			closeNativeMultipart(native, i+1)
			return err
		}
	}
	if err := submitPreparedMultipart(&preparedMultipart{native: native}, submit); err != nil {
		return err
	}
	if consumeOriginal {
		closeMessageSlice(parts)
	}
	return nil
}

func submitSinglePartFromCopy(part *Message, submit multipartSubmitFunc) error {
	if part == nil {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if part.closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var native C.zlink_msg_t
	rc60, errno60 := C.zlink_msg_init(&native)
	if err := configErrorFromCall(rc60, errno60); err != nil {
		return err
	}
	// HOT PATH: public Send/Publish(...).Message(message).Submit(...) reaches
	// this helper for every single-part send. Keep the copy before native submit:
	// Go promises that Message(...) preserves the caller message when submit
	// fails, and the native send call may not leave enough payload state to move
	// the frame back after a failure.
	rc61, errno61 := C.zlink_msg_copy(&native, &part.msg)
	if err := configErrorFromCall(rc61, errno61); err != nil {
		rc62, errno62 := C.zlink_msg_close(&native)
		_ = configErrorFromCall(rc62, errno62)
		return err
	}
	err := submit(&native, 1)
	if err != nil {
		return err
	}
	rc63, errno63 := C.zlink_msg_close(&part.msg)
	_ = configErrorFromCall(rc63, errno63)
	part.moved()
	return nil
}

func submitSinglePartMoved(part *Message, submit multipartSubmitFunc) error {
	if part == nil {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if part.closed {
		return &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
	}
	var native C.zlink_msg_t
	rc64, errno64 := C.zlink_msg_init(&native)
	if err := configErrorFromCall(rc64, errno64); err != nil {
		return err
	}
	// HOT PATH: public MoveMessage(...) explicitly transfers ownership at submit
	// time. Keep this as the no-copy path, separate from Message(...), whose
	// failure contract requires preserving the caller's message.
	rc65, errno65 := C.zlink_msg_move(&native, &part.msg)
	if err := configErrorFromCall(rc65, errno65); err != nil {
		rc66, errno66 := C.zlink_msg_close(&native)
		_ = configErrorFromCall(rc66, errno66)
		return err
	}
	err := submit(&native, 1)
	part.moved()
	return err
}

func submitSinglePartFromBytes(data []byte, submit multipartSubmitFunc) error {
	var native C.zlink_msg_t
	if err := initNativeMessageFromBytes(&native, data); err != nil {
		return err
	}
	return submit(&native, 1)
}

func submitMultipartFromBuilderParts(parts []sendBuilderPart, submit multipartSubmitFunc) error {
	if len(parts) == 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if len(parts) == 1 {
		if parts[0].bytes {
			return submitSinglePartFromBytes(parts[0].data, submit)
		}
		if parts[0].move {
			return submitSinglePartMoved(parts[0].message, submit)
		}
		return submitSinglePartFromCopy(parts[0].message, submit)
	}
	native := make([]C.zlink_msg_t, len(parts))
	movedParts := make([]*Message, len(parts))
	initialized := 0
	restorePreparation := func(err error) error {
		prepared := &preparedMultipart{
			native: native[:initialized],
			parts:  movedParts[:initialized],
		}
		if restoreErr := prepared.restore(); restoreErr != nil {
			return restoreErr
		}
		return err
	}
	for i, part := range parts {
		if part.bytes {
			if err := initNativeMessageFromBytes(&native[i], part.data); err != nil {
				return restorePreparation(err)
			}
			initialized = i + 1
			continue
		}
		if part.message == nil {
			return restorePreparation(&ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)})
		}
		if part.message.closed {
			return restorePreparation(&ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)})
		}
		rc67, errno67 := C.zlink_msg_init(&native[i])
		if err := configErrorFromCall(rc67, errno67); err != nil {
			return restorePreparation(err)
		}
		initialized = i + 1
		if part.move {
			rc68, errno68 := C.zlink_msg_move(&native[i], &part.message.msg)
			if err := configErrorFromCall(rc68, errno68); err != nil {
				return restorePreparation(err)
			}
			movedParts[i] = part.message
		} else {
			rc69, errno69 := C.zlink_msg_copy(&native[i], &part.message.msg)
			if err := configErrorFromCall(rc69, errno69); err != nil {
				return restorePreparation(err)
			}
		}
	}

	prepared := &preparedMultipart{native: native, parts: movedParts}
	err := submitPreparedMultipart(prepared, submit)
	for _, part := range parts {
		// Bytes parts are represented only by the temporary native message. The
		// caller-owned Message cleanup below applies only to message-backed parts.
		if part.message == nil {
			continue
		}
		if part.move {
			part.message.moved()
			continue
		}
		if err == nil {
			rc70, errno70 := C.zlink_msg_close(&part.message.msg)
			_ = configErrorFromCall(rc70, errno70)
			part.message.moved()
		}
	}
	return err
}
