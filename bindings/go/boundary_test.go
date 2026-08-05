package zlink_test

import (
	"context"
	"errors"
	"go/ast"
	"go/parser"
	"go/token"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"testing"
	"time"

	zlink "zlink.systems/zlink/v11"
)

func TestRoutingIDBoundary(t *testing.T) {
	valid := strings.Repeat("a", 255)
	if got := zlink.NewRoutingID([]byte(valid)); got.Size() != 255 {
		t.Fatalf("NewRoutingID(255 bytes).Size() = %d, want 255", got.Size())
	}

	invalid := strings.Repeat("b", 256)
	if got := zlink.NewRoutingID([]byte(invalid)); got.Size() != 0 {
		t.Fatalf("NewRoutingID(256 bytes).Size() = %d, want 0", got.Size())
	}
}

func TestDurationOverflowFailsFast(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	defer socket.Close()

	if err := socket.SetReceiveTimeout((1<<31)*time.Millisecond + time.Second); err == nil {
		t.Fatalf("SetReceiveTimeout() overflow should fail")
	}
}

func TestDurationMaxInt32MillisAccepted(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	defer socket.Close()

	maxDuration := time.Duration((1<<31)-1) * time.Millisecond
	if err := socket.SetReceiveTimeout(maxDuration); err != nil {
		t.Fatalf("SetReceiveTimeout(max int32 millis) error = %v", err)
	}
}

func TestNullByteValidation(t *testing.T) {
	ctx := newContext(t)
	defer ctx.Close()

	pair, _ := ctx.PairSocket()
	defer pair.Close()

	if err := pair.Bind("inproc://bad\x00endpoint"); err == nil {
		t.Fatalf("Bind() with null byte should fail")
	}
	if err := pair.Bind("tcp://127.0.0.1/" + strings.Repeat("a", 256)); err == nil {
		t.Fatalf("Bind() with oversized endpoint should fail")
	}

	sub, _ := ctx.SubSocket()
	defer sub.Close()

	if err := sub.SetSubscription("bad\x00topic"); err == nil {
		t.Fatalf("SetSubscription() with null byte should fail")
	}
	if err := sub.UnsetSubscription("bad\x00topic"); err == nil {
		t.Fatalf("UnsetSubscription() with null byte should fail")
	}
}

func TestNilInputValidation(t *testing.T) {
	if got := zlink.NewRoutingID(nil); got.Size() != 0 {
		t.Fatalf("NewRoutingID(nil).Size() = %d, want 0", got.Size())
	}

	ctx := newContext(t)
	defer ctx.Close()

	socket, _ := ctx.PairSocket()
	defer socket.Close()

	if _, err := socket.Recv(nil, zlink.RecvFlagsNone); err == nil {
		t.Fatal("Recv(nil) should fail")
	} else {
		var recvErr *zlink.RecvError
		if !errors.As(err, &recvErr) || recvErr.InternalErrno() != int(syscall.EFAULT) {
			t.Fatalf("Recv(nil) error = %v, want *RecvError/EFAULT", err)
		}
	}
	if _, err := socket.Send().Message(nil).Submit(context.Background()); err == nil {
		t.Fatalf("Send(nil) should fail")
	}
	stream, _ := ctx.StreamSocket()
	defer stream.Close()

	if err := stream.OnPacket(nil); err == nil {
		t.Fatalf("OnPacket(nil) should fail")
	}
}

func TestSamplesAndPerfUseOnlyPublicBindingContract(t *testing.T) {
	var violations []string
	for _, root := range []string{"samples", "perf"} {
		err := filepath.WalkDir(root, func(path string, entry os.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if entry.IsDir() {
				if entry.Name() == "build" || entry.Name() == "results" {
					return filepath.SkipDir
				}
				return nil
			}
			if filepath.Ext(path) != ".go" {
				return nil
			}

			file, err := parser.ParseFile(token.NewFileSet(), path, nil, parser.ImportsOnly)
			if err != nil {
				return err
			}
			for _, imported := range file.Imports {
				value := strings.Trim(imported.Path.Value, `"`)
				if value == "C" ||
					value == "unsafe" ||
					strings.HasPrefix(value, "zlink.systems/zlink/v11/internal/") {
					violations = append(violations, path+":"+value)
				}
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	if len(violations) != 0 {
		t.Fatalf("samples/perf must use public binding contract only: %v", violations)
	}
}

func TestRootProjectionCoversContractExports(t *testing.T) {
	contractNames := exportedTopLevelNames(t, "contracts")
	rootNames := exportedTopLevelNames(t, ".")
	var missing []string
	var extra []string

	for name := range contractNames {
		if _, ok := rootNames[name]; !ok {
			missing = append(missing, name)
		}
	}
	for name := range rootNames {
		if _, ok := contractNames[name]; !ok {
			extra = append(extra, name)
		}
	}
	sort.Strings(missing)
	sort.Strings(extra)

	if len(missing) != 0 {
		t.Fatalf("root package must project every public contract symbol: %v", missing)
	}
	if len(extra) != 0 {
		t.Fatalf("root package must not define symbols outside the public contract: %v", extra)
	}
}

func exportedTopLevelNames(t *testing.T, dir string) map[string]struct{} {
	t.Helper()
	names := make(map[string]struct{})
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}

	for _, entry := range entries {
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".go" ||
			strings.HasSuffix(entry.Name(), "_test.go") {
			continue
		}
		path := filepath.Join(dir, entry.Name())
		file, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		for _, decl := range file.Decls {
			switch decl := decl.(type) {
			case *ast.GenDecl:
				for _, spec := range decl.Specs {
					for _, name := range exportedSpecNames(spec) {
						names[name] = struct{}{}
					}
				}
			case *ast.FuncDecl:
				if decl.Recv == nil && decl.Name.IsExported() {
					names[decl.Name.Name] = struct{}{}
				}
			}
		}
	}

	return names
}

func exportedSpecNames(spec ast.Spec) []string {
	switch spec := spec.(type) {
	case *ast.TypeSpec:
		if spec.Name.IsExported() {
			return []string{spec.Name.Name}
		}
	case *ast.ValueSpec:
		var names []string
		for _, name := range spec.Names {
			if name.IsExported() {
				names = append(names, name.Name)
			}
		}
		return names
	}
	return nil
}
