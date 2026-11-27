package zlink_test

import (
	"flag"
	"os"
	"reflect"
	"regexp"
	"sort"
	"strings"
	"testing"

	zlink "zlink.systems/zlink"
)

var updateMethodSets = flag.Bool("update-method-sets", false, "rewrite testdata/method_sets.golden")

// handleTypes are the concrete handle types the root package exposes. Their
// exported method sets are the public contract: the spec names them, and this
// test keeps the implementation from drifting from the recorded set.
var handleTypes = map[string]reflect.Type{
	"Context":       reflect.TypeOf((*zlink.Context)(nil)),
	"PairSocket":    reflect.TypeOf((*zlink.PairSocket)(nil)),
	"PubSocket":     reflect.TypeOf((*zlink.PubSocket)(nil)),
	"SubSocket":     reflect.TypeOf((*zlink.SubSocket)(nil)),
	"DealerSocket":  reflect.TypeOf((*zlink.DealerSocket)(nil)),
	"RouterSocket":  reflect.TypeOf((*zlink.RouterSocket)(nil)),
	"XPubSocket":    reflect.TypeOf((*zlink.XPubSocket)(nil)),
	"XSubSocket":    reflect.TypeOf((*zlink.XSubSocket)(nil)),
	"StreamSocket":  reflect.TypeOf((*zlink.StreamSocket)(nil)),
	"Poller":        reflect.TypeOf((*zlink.Poller)(nil)),
	"Timer":         reflect.TypeOf((*zlink.Timer)(nil)),
	"SocketMonitor": reflect.TypeOf((*zlink.SocketMonitor)(nil)),
	"Message":       reflect.TypeOf((*zlink.Message)(nil)),
}

var qualifier = regexp.MustCompile(`\b(native|contracts)\.`)

func renderMethodSets() string {
	names := make([]string, 0, len(handleTypes))
	for name := range handleTypes {
		names = append(names, name)
	}
	sort.Strings(names)
	var b strings.Builder
	for _, name := range names {
		typ := handleTypes[name]
		for i := 0; i < typ.NumMethod(); i++ {
			m := typ.Method(i)
			sig := strings.TrimPrefix(m.Type.String(), "func(")
			// Value types are owned by contracts and the handle types by
			// internal/native; the package qualifier is not part of the contract.
			sig = qualifier.ReplaceAllString(sig, "")
			b.WriteString(name + "." + m.Name + " " + sig + "\n")
		}
	}
	return b.String()
}

func TestHandleMethodSetsMatchRecordedContract(t *testing.T) {
	const golden = "testdata/method_sets.golden"
	got := renderMethodSets()
	if *updateMethodSets {
		if err := os.MkdirAll("testdata", 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(golden, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(golden)
	if err != nil {
		t.Fatalf("read %s: %v (run with -update-method-sets)", golden, err)
	}
	if string(want) != got {
		t.Fatalf("handle method sets differ from %s; review the public API change and run with -update-method-sets", golden)
	}
}

// Every method the spec writes as `(*Type).Method` must exist on the type.
func TestSpecNamedMethodsExist(t *testing.T) {
	spec, err := os.ReadFile("../doc/spec/go/README.ko.md")
	if err != nil {
		t.Fatal(err)
	}
	found := regexp.MustCompile(`\(\*([A-Za-z]+)\)\.([A-Za-z]+)`).FindAllStringSubmatch(string(spec), -1)
	if len(found) == 0 {
		t.Fatal("no (*Type).Method references found in the Go spec")
	}
	for _, m := range found {
		typ, ok := handleTypes[m[1]]
		if !ok {
			t.Errorf("spec names unknown handle type %s", m[1])
			continue
		}
		if _, ok := typ.MethodByName(m[2]); !ok {
			t.Errorf("spec names %s.%s but the type has no such method", m[1], m[2])
		}
	}
}
