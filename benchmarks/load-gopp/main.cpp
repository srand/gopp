// benchmarks/load ported to go++: a closed-loop HTTP load generator where
// each of c workers sends a GET with http::Get, reads the response, and
// repeats. The default transport keeps one idle keep-alive connection per
// worker.
//
//     load -addr 127.0.0.1:8080 -path / -c 64 -d 5s -warmup 1s
//
// Prints one JSON object with throughput and latency percentiles, in the
// same format as the Go version.
#include <gopp/encoding/json.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/slice.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>

using namespace gopp;

namespace {

struct result {
    int64 requests;      // in the measured period
    int64 all_requests;  // including the warmup
    int64 errors;
    double seconds;
    double rps;
    double p50_us;
    double p90_us;
    double p99_us;
    double p999_us;
    double max_us;
};

// Sends requests until end. Returns the latencies of the requests that
// started after measure_from, and the count of all requests.
std::tuple<slice<time::Duration>, int64, error> run(const string& url, time::Time measure_from, time::Time end) {
    auto lats = slice<time::Duration>::make(0, 1 << 16);
    int64 n = 0;
    for (;;) {
        auto t0 = time::Now();
        if (!t0.Before(end)) return {lats, n, nullptr};
        auto [resp, err] = http::Get(url);
        if (err) return {lats, n, err};
        if (resp->StatusCode != http::StatusOK) {
            resp->Body->Close();
            return {lats, n, fmt::Errorf("unexpected status {}", resp->Status)};
        }
        auto [_, cerr] = io::Copy(io::Discard(), *resp->Body);
        resp->Body->Close();
        if (cerr) return {lats, n, cerr};
        n++;
        if (!t0.Before(measure_from)) lats = append(std::move(lats), time::Since(t0));
    }
}

}  // namespace

int main(int argc, char** argv) {
    string addr, path;
    isize conns;
    time::Duration dur, warmup;
    flag::StringVar(&addr, "addr", "127.0.0.1:8080", "server address");
    flag::StringVar(&path, "path", "/", "request path");
    flag::IntVar(&conns, "c", 64, "connections");
    flag::DurationVar(&dur, "d", 5 * time::Second, "measured duration");
    flag::DurationVar(&warmup, "warmup", time::Second, "unmeasured warmup");
    flag::Parse(argc, argv);

    http::DefaultTransport()->MaxIdleConnsPerHost = static_cast<int>(conns);
    string url = "http://" + addr + path;
    auto start = time::Now();
    auto measure_from = start.Add(warmup);
    auto end = measure_from.Add(dur);

    std::atomic<int64> all{0}, errs{0};
    sync::Mutex mu;
    slice<time::Duration> lats;
    sync::WaitGroup wg;
    for (isize i = 0; i < conns; i++) {
        wg.Go([&] {
            auto [local, n, err] = run(url, measure_from, end);
            all += n;
            if (err) {
                errs++;
                fmt::Println(os::stderr, "{}", err);
            }
            std::lock_guard l(mu);
            lats = append_span<time::Duration>(lats, local);
        });
    }
    wg.Wait();

    std::sort(lats.begin(), lats.end());
    auto pct = [&](double q) {
        if (lats.empty()) return 0.0;
        return static_cast<double>(lats[static_cast<std::size_t>(q * static_cast<double>(lats.len() - 1))].ns) / 1e3;
    };
    result r{
        .requests = static_cast<int64>(lats.len()),
        .all_requests = all.load(),
        .errors = errs.load(),
        .seconds = dur.Seconds(),
        .rps = static_cast<double>(lats.len()) / dur.Seconds(),
        .p50_us = pct(0.50),
        .p90_us = pct(0.90),
        .p99_us = pct(0.99),
        .p999_us = pct(0.999),
        .max_us = pct(1),
    };
    json::NewEncoder(os::stdout).Encode(r);
}
