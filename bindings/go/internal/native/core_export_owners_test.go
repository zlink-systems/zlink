// SPDX-License-Identifier: MPL-2.0

package native

import (
	"go/ast"
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func TestEveryPackagedCoreExportHasAnOwner(t *testing.T) {
	export := regexp.MustCompile(`(?m)^ZLINK_EXPORT\s+[^;#]*?\b(zlink_[A-Za-z0-9_]+)\s*\(`)
	include := os.DirFS(filepath.Join(moduleRoot(t), "include"))
	exports := map[string]string{}
	err := fs.WalkDir(include, ".", func(path string, entry fs.DirEntry, err error) error {
		if err != nil || filepath.Ext(path) != ".h" {
			return err
		}
		content, err := fs.ReadFile(include, path)
		if err != nil {
			return err
		}
		for _, match := range export.FindAllSubmatch(content, -1) {
			exports[string(match[1])] = path
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(exports) == 0 {
		t.Fatal("no Core exports found")
	}

	// These Core entries have no Go operation under the current contract.
	withoutGoCall := map[string]string{
		"zlink_msg_init_data":  "Go creates Core-owned buffers instead of attaching managed memory",
		"zlink_set_sub_option": "the only SUB option, TopicsCount, is read-only",
	}
	for name, reason := range withoutGoCall {
		if _, ok := exports[name]; !ok {
			t.Errorf("stale exception for %s (%s)", name, reason)
		}
		delete(exports, name)
	}

	coreCall := regexp.MustCompile(`\b(zlink_[A-Za-z0-9_]+)\s*\(`)
	files, err := filepath.Glob(filepath.Join(bindingRoot(t), "*.go"))
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range files {
		if strings.HasSuffix(path, "_test.go") {
			continue
		}
		file, err := parser.ParseFile(token.NewFileSet(), path, nil, parser.ParseComments)
		if err != nil {
			t.Fatal(err)
		}
		ast.Inspect(file, func(node ast.Node) bool {
			switch n := node.(type) {
			case *ast.CallExpr:
				if sel, ok := n.Fun.(*ast.SelectorExpr); ok {
					if pkg, ok := sel.X.(*ast.Ident); ok && pkg.Name == "C" {
						delete(exports, sel.Sel.Name)
					}
				}
			case *ast.GenDecl:
				if n.Tok == token.IMPORT && len(n.Specs) == 1 {
					spec := n.Specs[0].(*ast.ImportSpec)
					if spec.Path.Value == `"C"` {
						for _, match := range coreCall.FindAllStringSubmatch(n.Doc.Text()+spec.Doc.Text(), -1) {
							delete(exports, match[1])
						}
					}
				}
			}
			return true
		})
	}
	for symbol, path := range exports {
		t.Errorf("%s exports %s without an owning Go call or cgo bridge", path, symbol)
	}
}
