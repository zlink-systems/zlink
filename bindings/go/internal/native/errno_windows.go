//go:build windows

package native

import "syscall"

// Core reports portable C errno values. Windows exposes the corresponding
// values through syscall.Errno with its platform-specific encoding.
func normalizeNativeErrno(errno int) int {
	switch errno {
	case 4:
		return int(syscall.EINTR)
	case 5:
		return int(syscall.EIO)
	case 11:
		return int(syscall.EAGAIN)
	case 12:
		return int(syscall.ENOMEM)
	case 13:
		return int(syscall.EACCES)
	case 14:
		return int(syscall.EFAULT)
	case 22:
		return int(syscall.EINVAL)
	case 38:
		return int(syscall.ENAMETOOLONG)
	case 111:
		return int(syscall.ECONNREFUSED)
	case 126:
		return int(syscall.ENOTCONN)
	case 138:
		return int(syscall.ETIMEDOUT)
	default:
		return errno
	}
}

func denormalizeNativeErrno(errno int) int {
	switch errno {
	case int(syscall.EINTR):
		return 4
	case int(syscall.EIO):
		return 5
	case int(syscall.EAGAIN):
		return 11
	case int(syscall.ENOMEM):
		return 12
	case int(syscall.EACCES):
		return 13
	case int(syscall.EFAULT):
		return 14
	case int(syscall.EINVAL):
		return 22
	case int(syscall.ENAMETOOLONG):
		return 38
	case int(syscall.ECONNREFUSED):
		return 111
	case int(syscall.ENOTCONN):
		return 126
	case int(syscall.ETIMEDOUT):
		return 138
	default:
		return errno
	}
}
