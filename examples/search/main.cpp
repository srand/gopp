// search: the replicated search of Rob Pike's "Go Concurrency Patterns",
// with contexts. A query goes to three kinds of backend (web, images,
// video) at once, each kind to several replicas, and the first replica to
// answer wins. The whole search has a deadline: kinds that miss it are left
// out. The backends are fakes that take a random time, up to -slow.
//
//     $ search golang
//     image result for "golang" from image2 (12ms)
//     web result for "golang" from web1 (11ms)
//     video result for "golang" from video2 (14ms)
//     3 of 3 kinds in 16ms; canceled 6 slower replicas
//
//     $ search -replicas 1 -timeout 40ms golang     # one replica each: some time out
//
// When the search is over, its context is canceled, and the replicas that
// are still working stop: each waits in a select on its work and on
// ctx->Done(). A WaitGroup makes sure that none is left behind.
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <atomic>
#include <random>
#include <vector>

using namespace gopp;

time::Duration slowest = 80 * time::Millisecond;
std::atomic<int> canceled{0};

struct Result {
    string kind;
    string text;
};

// A fake backend: answers after a random time, unless ctx ends first.
std::pair<Result, error> query(const ptr<context::Context>& ctx, string kind, string replica, string q) {
    thread_local std::mt19937_64 rng{std::random_device{}()};
    auto took = time::Duration(std::uniform_int_distribution<int64>(0, slowest.Nanoseconds())(rng));
    error err;
    select(recv(time::After(took), [] {}), recv(ctx->Done(), [&] { err = ctx->Err(); }));
    if (err) {
        canceled++;
        return {{}, err};
    }
    return {{kind, fmt::Sprint("{} result for \"{}\" from {} ({})", kind, q, replica, took.Round(time::Millisecond))},
            nullptr};
}

// Asks every replica of a kind and returns the first answer. The channel
// has room for all of them, so the ones that lose don't block on their send.
Result first(const ptr<context::Context>& ctx, sync::WaitGroup& replicas, string kind, int n, string q) {
    auto answers = makeChan<Result>(n);
    for (int i = 1; i <= n; i++) {
        replicas.Go([=] {
            auto [r, err] = query(ctx, kind, fmt::Sprint("{}{}", kind, i), q);
            if (!err) answers.send(r);
        });
    }
    Result r;
    select(recv(answers, [&](Result a) { r = a; }), recv(ctx->Done(), [] {}));
    return r;
}

int main(int argc, char** argv) {
    time::Duration timeout = 50 * time::Millisecond;
    isize n = 3;
    flag::DurationVar(&timeout, "timeout", timeout, "how long the search may take");
    flag::DurationVar(&slowest, "slow", slowest, "the longest a backend takes");
    flag::IntVar(&n, "replicas", n, "the replicas of each backend");
    flag::Parse(argc, argv);
    if (flag::Args().size() != 1 || n < 1) {
        fmt::Println(os::Stderr, "usage: search [-timeout d] [-slow d] [-replicas n] query");
        return 2;
    }
    string q = flag::Args()[0];

    time::Time start = time::Now();
    auto [ctx, cancel] = context::WithTimeout(context::Background(), timeout);
    sync::WaitGroup replicas;

    const std::vector<string> kinds = {"web", "image", "video"};
    auto results = makeChan<Result>(kinds.size());
    sync::WaitGroup searches;
    for (const string& kind : kinds) {
        searches.Go([&, kind] { results.send(first(ctx, replicas, kind, n, q)); });
    }

    int answered = 0;
    for (std::size_t i = 0; i < kinds.size(); i++) {
        Result r = results.recv();
        if (r.text.empty()) continue;  // this kind missed the deadline
        fmt::Println("{}", r.text);
        answered++;
    }
    time::Duration elapsed = time::Since(start);
    if (ctx->Err()) fmt::Println("timed out after {}", timeout);

    // The search is over: stop the replicas still at work.
    cancel();
    searches.Wait();
    replicas.Wait();
    fmt::Println("{} of {} kinds in {}; canceled {} slower replicas", answered, kinds.size(),
                 elapsed.Round(time::Millisecond), canceled.load());
    return answered == static_cast<int>(kinds.size()) ? 0 : 1;
}
