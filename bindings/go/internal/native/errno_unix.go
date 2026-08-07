//go:build !windows

package native

func normalizeNativeErrno(errno int) int {
	return errno
}

func denormalizeNativeErrno(errno int) int {
	return errno
}
