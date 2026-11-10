// The Go counterpart of examples/hello: same routes, same responses.
package main

import (
	"fmt"
	"net/http"
	"os"
)

func main() {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /{$}", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "Hello, World!\n")
	})

	mux.HandleFunc("GET /hello/{name}", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "Hello, %s!\n", r.PathValue("name"))
	})

	addr := ":8080"
	if len(os.Args) > 1 {
		addr = os.Args[1]
	}
	srv := &http.Server{Addr: addr, Handler: mux}
	fmt.Println("listening on", srv.Addr)
	if err := srv.ListenAndServe(); err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
}
