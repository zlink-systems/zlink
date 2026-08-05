package native

import (
	"sync"
	"testing"
	"unsafe"
)

func TestExternalRequestProgressRegistryReferenceCounting(t *testing.T) {
	var token byte
	handle := unsafe.Pointer(&token)

	const workers = 8
	const iterations = 200
	var group sync.WaitGroup
	group.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer group.Done()
			for j := 0; j < iterations; j++ {
				acquireExternalRequestProgress(handle, nil)
				if !externalRequestProgressActive(handle) {
					t.Errorf("registry reported inactive after acquire")
					return
				}
				releaseExternalRequestProgress(handle, nil)
			}
		}()
	}
	group.Wait()

	if externalRequestProgressActive(handle) {
		t.Fatalf("registry retained progress ownership after balanced release")
	}
}
