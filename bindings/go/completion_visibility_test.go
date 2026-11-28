package zlink_test

import (
	"go/ast"
	"go/parser"
	"go/token"
	"testing"
)

func TestCompletionRecordKindsAreInternal(t *testing.T) {
	for _, dir := range []string{".", "contracts"} {
		packages, err := parser.ParseDir(token.NewFileSet(), dir, nil, 0)
		if err != nil {
			t.Fatal(err)
		}
		for _, name := range []string{"CompletionKind", "CompletionSend", "CompletionRequest", "CompletionWritable"} {
			for _, pkg := range packages {
				for path, file := range pkg.Files {
					if obj := file.Scope.Lookup(name); obj != nil && ast.IsExported(name) {
						t.Errorf("%s exports %s", path, name)
					}
				}
			}
		}
	}
}
