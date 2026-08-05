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

type multipartSubmitFunc func(*C.zlink_msg_t, C.zlink_part_flag_t) error
type multipartRecvFunc func(*C.zlink_msg_t, *C.zlink_part_flag_t, C.zlink_recv_flags_t) error

func prepareMultipart(parts []*Message) (*preparedMultipart, error) {
	if len(parts) == 0 {
		return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	native := make([]C.zlink_msg_t, len(parts))
	for i, part := range parts {
		if part == nil {
			return nil, &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
		}
		if part.closed {
			return nil, &ConfigError{Result: ConfigInvalidHandle, nativeErrno: int(C.EFAULT)}
		}
		if err := configErrorFromResult(C.zlink_msg_init(&native[i])); err != nil {
			closeNativeMultipart(native, i)
			return nil, err
		}
		if err := configErrorFromResult(C.zlink_msg_move(&native[i], &part.msg)); err != nil {
			prepared := &preparedMultipart{native: native[:i+1], parts: parts[:i+1]}
			restoreErr := prepared.restore()
			if restoreErr != nil {
				return nil, restoreErr
			}
			return nil, err
		}
	}
	return &preparedMultipart{native: native, parts: parts}, nil
}

func (p *preparedMultipart) ptr() *C.zlink_msg_t {
	if p == nil || len(p.native) == 0 {
		return nil
	}
	return &p.native[0]
}

func (p *preparedMultipart) count() C.size_t {
	if p == nil {
		return 0
	}
	return C.size_t(len(p.native))
}

func (p *preparedMultipart) commit() {
	if p == nil {
		return
	}
	for _, part := range p.parts {
		if part != nil {
			part.moved()
		}
	}
}

func (p *preparedMultipart) restore() error {
	if p == nil {
		return nil
	}
	for i, part := range p.parts {
		if part == nil {
			continue
		}
		if err := configErrorFromResult(C.zlink_msg_move(&part.msg, &p.native[i])); err != nil {
			closeNativeMultipart(p.native, len(p.native))
			return err
		}
	}
	closeNativeMultipart(p.native, len(p.native))
	return nil
}

func closeConsumedParts(parts []*Message) {
	for _, part := range parts {
		if part != nil {
			_ = part.Close()
		}
	}
}

func submitPreparedMultipart(prepared *preparedMultipart, submit multipartSubmitFunc) error {
	if prepared == nil || len(prepared.native) == 0 {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	for i := range prepared.native {
		partFlag := C.zlink_part_flag_t(C.ZLINK_PART_FINAL)
		if i+1 < len(prepared.native) {
			partFlag = C.ZLINK_PART_MORE
		}
		if err := submit(&prepared.native[i], partFlag); err != nil {
			if i+1 < len(prepared.native) {
				closeNativeMultipart(prepared.native[i+1:], len(prepared.native)-(i+1))
			}
			return err
		}
	}
	return nil
}

func initNativeMessageFromBytes(native *C.zlink_msg_t, data []byte) error {
	if native == nil {
		return &ConfigError{Result: ConfigInvalidArgument, nativeErrno: int(C.EINVAL)}
	}
	if err := configErrorFromResult(C.zlink_msg_init_size(native, C.size_t(len(data)))); err != nil {
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
	cloned, err := cloneParts(parts)
	if err != nil {
		return err
	}
	prepared, err := prepareMultipart(cloned)
	if err != nil {
		closeMessageSlice(cloned)
		return err
	}
	err = submitPreparedMultipart(prepared, submit)
	prepared.commit()
	if err != nil {
		return err
	}
	if consumeOriginal {
		closeConsumedParts(parts)
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
	if err := configErrorFromResult(C.zlink_msg_init(&native)); err != nil {
		return err
	}
	// HOT PATH: public Send/Publish(...).Message(message).Submit(...) reaches
	// this helper for every single-part send. Keep the copy before native submit:
	// Go promises that Message(...) preserves the caller message when submit
	// fails, and the native send call may not leave enough payload state to move
	// the frame back after a failure.
	if err := configErrorFromResult(C.zlink_msg_copy(&native, &part.msg)); err != nil {
		_ = configErrorFromResult(C.zlink_msg_close(&native))
		return err
	}
	err := submit(&native, C.zlink_part_flag_t(C.ZLINK_PART_FINAL))
	if err != nil {
		_ = configErrorFromResult(C.zlink_msg_close(&native))
		return err
	}
	_ = configErrorFromResult(C.zlink_msg_close(&part.msg))
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
	if err := configErrorFromResult(C.zlink_msg_init(&native)); err != nil {
		return err
	}
	// HOT PATH: public MoveMessage(...) explicitly transfers ownership at submit
	// time. Keep this as the no-copy path, separate from Message(...), whose
	// failure contract requires preserving the caller's message.
	if err := configErrorFromResult(C.zlink_msg_move(&native, &part.msg)); err != nil {
		_ = configErrorFromResult(C.zlink_msg_close(&native))
		return err
	}
	err := submit(&native, C.zlink_part_flag_t(C.ZLINK_PART_FINAL))
	if err != nil {
		_ = configErrorFromResult(C.zlink_msg_close(&native))
	}
	part.moved()
	return err
}

func submitSinglePartFromBytes(data []byte, submit multipartSubmitFunc) error {
	var native C.zlink_msg_t
	if err := initNativeMessageFromBytes(&native, data); err != nil {
		return err
	}
	err := submit(&native, C.zlink_part_flag_t(C.ZLINK_PART_FINAL))
	if err != nil {
		_ = configErrorFromResult(C.zlink_msg_close(&native))
	}
	return err
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
		if err := configErrorFromResult(C.zlink_msg_init(&native[i])); err != nil {
			return restorePreparation(err)
		}
		initialized = i + 1
		if part.move {
			if err := configErrorFromResult(C.zlink_msg_move(&native[i], &part.message.msg)); err != nil {
				return restorePreparation(err)
			}
			movedParts[i] = part.message
		} else {
			if err := configErrorFromResult(C.zlink_msg_copy(&native[i], &part.message.msg)); err != nil {
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
			_ = configErrorFromResult(C.zlink_msg_close(&part.message.msg))
			part.message.moved()
		}
	}
	return err
}
