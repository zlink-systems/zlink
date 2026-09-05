package native

import (
	"bytes"
	"context"
	"fmt"
	"testing"
)

func TestSendRetryStoragePreservesSourcesOnPreparationFailure(t *testing.T) {
	for _, move := range []bool{false, true} {
		t.Run(fmt.Sprint(move), func(t *testing.T) {
			source, err := NewMessage([]byte("retained"))
			if err != nil {
				t.Fatal(err)
			}
			defer source.Close()
			if _, err := newSendRetryPayload([]sendBuilderPart{{message: source, move: move}, {message: nil}}); err == nil {
				t.Fatal("invalid later part accepted")
			}
			if source.closed || !bytes.Equal(source.Data(), []byte("retained")) {
				t.Fatal("preparation failure consumed earlier source")
			}
		})
	}
}

func TestSendBuilderPreservesMultipartAcrossInlineBoundary(t *testing.T) {
	for _, count := range []int{1, 2, 3, 8} {
		t.Run(fmt.Sprint(count), func(t *testing.T) {
			op := newSendBuilder(func(_ context.Context, parts []sendBuilderPart) error {
				if len(parts) != count {
					t.Fatalf("got %d parts, want %d", len(parts), count)
				}
				for i, part := range parts {
					if !part.bytes || !bytes.Equal(part.data, []byte{byte(i)}) {
						t.Fatalf("part %d changed", i)
					}
				}
				return nil
			})
			submit := op.Bytes([]byte{0})
			for i := 1; i < count; i++ {
				submit = submit.Bytes([]byte{byte(i)})
			}
			if err := submit.Submit(context.Background()); err != nil {
				t.Fatal(err)
			}
			if err := submit.Submit(context.Background()); err == nil {
				t.Fatal("builder accepted second Submit")
			}
		})
	}
}
