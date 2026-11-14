// panic: what a crash looks like. A pool of fibers fills orders; one order
// runs into a bug and the program dies as a Go program does: it prints the
// panic, then the goroutine's stack with each function, its file and line,
// and the goroutine that created it, and exits with status 2.
//
//     $ panic                        # gopp::panic, Go's panic(...)
//     $ panic -crash index           # a slice index out of range
//     $ panic -crash closed          # a send on a closed channel
//     $ panic -crash throw           # an exception that nobody catches
//     $ panic -crash nil             # a nil pointer dereference: SIGSEGV
//     $ GOTRACEBACK=all panic        # every goroutine's stack, with what it waits for
//
//     $ panic
//     order 1: 1 x item 1
//     ...
//     panic: order 7: 12 x gizmo, only 1 in stock
//
//     goroutine 1 [running]:
//     gopp::panic(...)
//             src/runtime/proc.cpp:28 +0x34
//     check_stock(...)
//             examples/panic/main.cpp:85 +0x101
//     fill(...)
//             examples/panic/main.cpp:111 +0x1c
//     main::{lambda()#1}::operator()() const
//             examples/panic/main.cpp:130 +0xab
//     gopp::sync::WaitGroup::Go<main::{lambda()#1}>(main::{lambda()#1}&&)::{lambda()#1}::operator()()
//             .../include/gopp/sync.hpp:165 +0x1c
//     created by void gopp::sync::WaitGroup::Go<main::{lambda()#1}>
//             .../include/gopp/sync.hpp:164 +0xe7
//
// Goroutine 1 is the first fiber: main runs on the main thread, which
// GOTRACEBACK=all lists as "thread TID (main)", waiting in
// sync.WaitGroup.Wait, with the other packers waiting for the lock that
// check_stock holds. There is no recover: a panic ends the program. An
// exception is reported from where it was thrown, before any destructor
// runs, so the stack shows the throw. GOTRACEBACK=none prints only the
// message, and crash dies of SIGABRT, for a core dump.
//
// Strip the program and the frames print as addresses instead, such as
// "? pc=0x40aaf8 (/tmp/panic+0x40aaf7)": addr2line -f -C -e examples_panic
// 0x40aaf7 on the unstripped program gives back check_stock and its line.
#include <gopp/chan.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/slice.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <map>
#include <vector>

using namespace gopp;

string crash = "panic";

struct Item : object {
    string name;
    int in_stock;
    Item(string n, int s) : name(std::move(n)), in_stock(s) {}
};

struct Order {
    int id;
    int item;  // an index into the catalog
    int quantity;
};

struct Receipt {
    int order;
    string line;
};

sync::Mutex mu;
std::vector<ptr<Item>> catalog;  // guarded by mu

// Takes the order's items from stock. Order 7 is the one with the bug.
void check_stock(const Order& o) {
    std::lock_guard lock(mu);
    if (o.id == 7) {
        if (crash == "panic") {
            const Item& item = *catalog[o.item];
            panic(fmt::Sprint("order {}: {} x {}, only {} in stock", o.id, o.quantity, item.name, item.in_stock)
                      .view());
        } else if (crash == "index") {
            // Go's s[i]: a slice checks its index.
            auto counts = slice<int>::make(catalog.size());
            counts[o.item + 10]++;
        } else if (crash == "closed") {
            auto done = make_chan<int>(1);
            done.close();
            done.send(o.id);
        } else if (crash == "throw") {
            // std::vector::at throws std::out_of_range.
            catalog.at(o.item + 10)->in_stock -= o.quantity;
        } else if (crash == "nil") {
            std::map<int, ptr<Item>> by_id;
            ptr<Item> item = by_id[o.item];  // nil: no such item
            item->in_stock -= o.quantity;
        } else {
            fmt::Println(os::Stderr, "panic: unknown -crash {}", crash);
            os::Exit(2);
        }
    }
    catalog[o.item]->in_stock -= o.quantity;
}

Receipt fill(const Order& o) {
    check_stock(o);
    time::Sleep(10 * time::Millisecond);  // packing
    return Receipt{o.id, fmt::Sprint("order {}: {} x item {}", o.id, o.quantity, o.item)};
}

int main(int argc, char** argv) {
    flag::StringVar(&crash, "crash", crash, "how order 7 crashes: panic, index, closed, throw or nil");
    flag::Parse(argc, argv);

    catalog = {make_ptr<Item>("widget", 100), make_ptr<Item>("gizmo", 3), make_ptr<Item>("doohickey", 40)};

    auto orders = make_chan<Order>();
    auto receipts = make_chan<Receipt>();

    // The packers: a pool of fibers that fill the orders.
    sync::WaitGroup packers;
    for (int i = 0; i < 4; i++) {
        packers.Go([orders, receipts] {
            for (const Order& o : orders) {
                receipts.send(fill(o));
            }
        });
    }

    // The printer, which gets the receipts.
    auto printed = make_chan<empty>();
    go([receipts, printed] {
        for (const Receipt& r : receipts) fmt::Println("{}", r.line);
        printed.close();
    });

    for (int id = 1; id <= 10; id++) {
        orders.send(Order{.id = id, .item = id % 3, .quantity = id == 7 ? 12 : 1});
    }
    orders.close();
    packers.Wait();
    receipts.close();
    printed.recv();
    fmt::Println("all orders filled");
}
