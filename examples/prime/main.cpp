// prime: the sieve of Eratosthenes as a pipeline of goroutines and channels.
// Each prime gets a fiber that removes its multiples from the stream.
//
//     $ prime
#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

using namespace gopp;

int main() {
    constexpr int count = 25;
    constexpr int limit = 200;

    auto numbers = makeChan<int>();
    go([numbers] {
        for (int n = 2; n <= limit; n++) numbers.send(n);
        numbers.close();
    });

    auto input = numbers;
    sync::WaitGroup filters;
    fmt::Print("First {} primes:", count);
    for (int i = 0; i < count; i++) {
        auto [prime, ok] = input.recv2();
        if (!ok) break;
        fmt::Print(" {}", prime);

        auto output = makeChan<int>();
        filters.Go([input, output, prime] {
            for (int n : input) {
                if (n % prime != 0) output.send(n);
            }
            output.close();
        });
        input = output;
    }
    fmt::Println("");

    // Drain the last stage, whose output nobody else reads, so that every
    // stage gets to the end of its input and closes its output.
    for (int n : input) (void)n;
    filters.Wait();
}
