// The smallest goc program. `goc go .` compiles add.c and links this package.
// hello_add has no body here; the body is the packed goc object.
package main

import "fmt"

func hello_add(a, b int32) int32

func main() {
	fmt.Printf("hello %d\n", hello_add(20, 22))
}
