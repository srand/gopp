// A closed-loop HTTP load generator: each of c workers sends a GET with
// http.Get, reads the response, and repeats. The default transport keeps
// one idle keep-alive connection per worker. The same client measures every
// server, so differences come from the servers.
//
//	load -addr 127.0.0.1:8080 -path / -c 64 -d 5s -warmup 1s
//
// Prints one JSON object with throughput and latency percentiles.
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"slices"
	"sync"
	"sync/atomic"
	"time"
)

type result struct {
	Requests    int64   `json:"requests"`     // in the measured period
	AllRequests int64   `json:"all_requests"` // including the warmup
	Errors      int64   `json:"errors"`
	Seconds     float64 `json:"seconds"`
	RPS         float64 `json:"rps"`
	P50us       float64 `json:"p50_us"`
	P90us       float64 `json:"p90_us"`
	P99us       float64 `json:"p99_us"`
	P999us      float64 `json:"p999_us"`
	MaxUs       float64 `json:"max_us"`
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8080", "server address")
	path := flag.String("path", "/", "request path")
	conns := flag.Int("c", 64, "connections")
	dur := flag.Duration("d", 5*time.Second, "measured duration")
	warmup := flag.Duration("warmup", time.Second, "unmeasured warmup")
	flag.Parse()

	http.DefaultTransport.(*http.Transport).MaxIdleConnsPerHost = *conns
	url := "http://" + *addr + *path
	start := time.Now()
	measureFrom := start.Add(*warmup)
	end := measureFrom.Add(*dur)

	var all, errs atomic.Int64
	var mu sync.Mutex
	var lats []time.Duration
	var wg sync.WaitGroup
	for i := 0; i < *conns; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			local, n, err := run(url, measureFrom, end)
			all.Add(n)
			if err != nil {
				errs.Add(1)
				fmt.Fprintln(os.Stderr, err)
			}
			mu.Lock()
			lats = append(lats, local...)
			mu.Unlock()
		}()
	}
	wg.Wait()

	slices.Sort(lats)
	pct := func(q float64) float64 {
		if len(lats) == 0 {
			return 0
		}
		return float64(lats[int(q*float64(len(lats)-1))].Nanoseconds()) / 1e3
	}
	r := result{
		Requests:    int64(len(lats)),
		AllRequests: all.Load(),
		Errors:      errs.Load(),
		Seconds:     dur.Seconds(),
		RPS:         float64(len(lats)) / dur.Seconds(),
		P50us:       pct(0.50),
		P90us:       pct(0.90),
		P99us:       pct(0.99),
		P999us:      pct(0.999),
		MaxUs:       pct(1),
	}
	json.NewEncoder(os.Stdout).Encode(r)
}

// run sends requests until end and returns the latencies of the requests
// that started after measureFrom, and the count of all requests.
func run(url string, measureFrom, end time.Time) ([]time.Duration, int64, error) {
	lats := make([]time.Duration, 0, 1<<16)
	var n int64
	for {
		t0 := time.Now()
		if !t0.Before(end) {
			return lats, n, nil
		}
		resp, err := http.Get(url)
		if err != nil {
			return lats, n, err
		}
		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			return lats, n, fmt.Errorf("unexpected status %s", resp.Status)
		}
		_, err = io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		if err != nil {
			return lats, n, err
		}
		n++
		if !t0.Before(measureFrom) {
			lats = append(lats, time.Since(t0))
		}
	}
}
