// SPDX-License-Identifier: MPL-2.0

package native

import (
	"go/ast"
	"go/parser"
	"go/token"
	"path/filepath"
	"testing"
)

func TestRouterHandoverUsesCommonOptionOwner(t *testing.T) {
	path := filepath.Join(bindingRoot(t), "socket_router.go")
	file, err := parser.ParseFile(token.NewFileSet(), path, nil, 0)
	if err != nil {
		t.Fatal(err)
	}
	ast.Inspect(file, func(node ast.Node) bool {
		sel, ok := node.(*ast.SelectorExpr)
		if ok && sel.Sel.Name == "ZLINK_OPT_RID_DUPLICATE_POLICY" {
			t.Error("RouterSocket must use CommonSocketOptions for the routing ID duplicate policy")
		}
		return true
	})
}
