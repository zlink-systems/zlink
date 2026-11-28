// SPDX-License-Identifier: MPL-2.0

package zlink_test

import (
	"go/ast"
	"go/parser"
	"go/token"
	"testing"
)

func TestMonitorSampleShowsPublicMonitorOperations(t *testing.T) {
	file, err := parser.ParseFile(token.NewFileSet(), "samples/monitor_recv_sample/main.go", nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	calls := map[string]bool{}
	ast.Inspect(file, func(node ast.Node) bool {
		if call, ok := node.(*ast.CallExpr); ok {
			if method, ok := call.Fun.(*ast.SelectorExpr); ok {
				calls[method.Sel.Name] = true
			}
		}
		return true
	})
	for _, name := range []string{"OpenSocketMonitor", "AddMonitor", "Wait", "Recv", "RemoveMonitor"} {
		if !calls[name] {
			t.Errorf("monitor receive sample hides %s", name)
		}
	}
}
