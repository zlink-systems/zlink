// SPDX-License-Identifier: MPL-2.0

package native

import (
	"errors"
	"syscall"
	"testing"
)

func TestConsumedMessageSubmit(t *testing.T) {
	for _, name := range []string{"send retention", "request staging", "publish move", "multipart reply"} {
		t.Run(name, func(t *testing.T) {
			message, err := NewMessage([]byte("consumed"))
			if err != nil {
				t.Fatal(err)
			}
			if err := message.Close(); err != nil {
				t.Fatal(err)
			}
			first, err := NewMessage([]byte("preserved"))
			if err != nil {
				t.Fatal(err)
			}
			defer first.Close()

			switch name {
			case "send retention":
				payload, buildErr := newSendRetryPayload([]sendBuilderPart{{message: message}})
				if payload != nil {
					payload.close()
				}
				err = buildErr
			case "request staging":
				err = submitMultipartFromBuilderParts([]sendBuilderPart{{message: message}}, nil)
			case "publish move":
				err = submitMultipartFromBuilderParts([]sendBuilderPart{
					{message: first, move: true}, {message: message, move: true},
				}, nil)
			case "multipart reply":
				err = submitMultipartFromClones([]*Message{first, message}, true, nil)
			}
			var configErr *ConfigError
			if !errors.As(err, &configErr) || configErr.Result != ConfigInvalidHandle || !errors.Is(err, syscall.EFAULT) {
				t.Errorf("error = %v, want ConfigInvalidHandle/EFAULT", err)
			}
			if message.Size() != 0 || len(message.Data()) != 0 || len(message.Bytes()) != 0 {
				t.Error("consumed message must read as empty")
			}
			if got := string(first.Data()); got != "preserved" {
				t.Errorf("earlier message = %q, want preserved", got)
			}
		})
	}
}
