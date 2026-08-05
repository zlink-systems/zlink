package native

/*
#include <stdint.h>
*/
import "C"

import (
	"fmt"
	"runtime/cgo"
	"unsafe"
)

//export zlinkGoThreadInvoke
func zlinkGoThreadInvoke(arg unsafe.Pointer) {
	handle := cgo.Handle(arg)
	state := handle.Value().(*threadState)
	defer func() {
		if recovered := recover(); recovered != nil {
			state.setErr(fmt.Errorf("thread handler panicked: %v", recovered))
		}
	}()
	state.target()
}
