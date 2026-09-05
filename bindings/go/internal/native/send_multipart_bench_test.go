package native

import (
	"context"
	"fmt"
	"runtime"
	"testing"
)

func BenchmarkManagedSendMultipart(b *testing.B) {
	for _, size := range []int{64, 65536} {
		b.Run(fmt.Sprint(size), func(b *testing.B) {
			ctx, err := NewContext()
			if err != nil {
				b.Fatal(err)
			}
			defer ctx.Close()
			a, err := ctx.PairSocket()
			if err != nil {
				b.Fatal(err)
			}
			defer a.Close()
			z, err := ctx.PairSocket()
			if err != nil {
				b.Fatal(err)
			}
			defer z.Close()
			if err := a.Bind("inproc://managed-send-multipart"); err != nil {
				b.Fatal(err)
			}
			if err := z.Connect("inproc://managed-send-multipart"); err != nil {
				b.Fatal(err)
			}
			data := make([]byte, size)
			exchange := func() {
				body, err := NewMessage(data)
				if err != nil {
					b.Fatal(err)
				}
				tail, err := NewMessageWithSize(0)
				if err != nil {
					b.Fatal(err)
				}
				if err := z.Send().MoveMessage(body).Message(tail).Submit(context.Background()); err != nil {
					b.Fatal(err)
				}
				var r Received
				if ok, err := a.Recv(&r, RecvFlagsNone); err != nil || !ok {
					b.Fatal(err)
				}
				r.Close()
			}
			exchange()
			calls := runtime.NumCgoCall()
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				exchange()
			}
			b.StopTimer()
			b.ReportMetric(float64(runtime.NumCgoCall()-calls)/float64(b.N), "cgo/op")
		})
	}
}
