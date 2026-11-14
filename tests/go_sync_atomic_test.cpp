// Ported from Go 1.27.3's sync/atomic/atomic_test.go and value_test.go,
// with Go's test names: each test of a type (Swap, Add, And, Or,
// CompareAndSwap, Load, Store, in the function and the method forms) runs
// for Int32, Uint32, Int64 and Uint64 (and so Uintptr). Left out: the tests of
// unsafe.Pointer functions, alignment and nil dereferences, which C++
// doesn't have, and of atomic.Value with values of different types (go++'s
// Value is typed).
#include <gopp/chan.hpp>
#include <gopp/ptr.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/sync/atomic.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <vector>

using namespace gopp;

namespace {

// The functions of each type, by name.
template <typename T>
struct funcs;
#define FUNCS(T, Name)                                                                         \
    template <>                                                                                \
    struct funcs<T> {                                                                          \
        using methodType = atomic::Name;                                                      \
        static T Swap(T* a, T v) { return atomic::Swap##Name(a, v); }                         \
        static T Add(T* a, T v) { return atomic::Add##Name(a, v); }                           \
        static T And(T* a, T v) { return atomic::And##Name(a, v); }                           \
        static T Or(T* a, T v) { return atomic::Or##Name(a, v); }                             \
        static bool CompareAndSwap(T* a, T o, T v) { return atomic::CompareAndSwap##Name(a, o, v); } \
        static T Load(const T* a) { return atomic::Load##Name(a); }                           \
        static void Store(T* a, T v) { atomic::Store##Name(a, v); }                           \
    };
FUNCS(int32, Int32)
FUNCS(uint32, Uint32)
FUNCS(int64, Int64)
FUNCS(uint64, Uint64)
#undef FUNCS

// The magic values around the word under test, which the operations must
// not touch.
template <typename T>
constexpr T magic() {
    if constexpr (sizeof(T) == 4) return T(0xdedbeef);
    return T(0xdeddeadbeefbeefULL);
}

// The loop over power-of-two values is meant to ensure that the operations
// apply to the full word size: delta = 1, 2, 4, ... while delta+delta >
// delta (computed unsigned, as signed overflow is undefined in C++).
template <typename T>
std::vector<T> powersOfTwo() {
    std::vector<T> v;
    using U = std::make_unsigned_t<T>;
    for (U d = 1;; d += d) {
        T delta = T(d), twice = T(U(d + d));
        if (!(twice > delta)) break;
        v.push_back(delta);
    }
    return v;
}

template <typename T>
class GoAtomic : public testing::Test {};

// (uintptr is uint64, on the 64-bit systems go++ runs on.)
using types = testing::Types<int32, uint32, int64, uint64>;
TYPED_TEST_SUITE(GoAtomic, types);

template <typename T, typename I>
struct boxed {
    T before;
    I i;
    T after;
};

TYPED_TEST(GoAtomic, Swap) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    T j = 0;
    for (T delta : powersOfTwo<T>()) {
        T k = funcs<T>::Swap(&x.i, delta);
        ASSERT_EQ(x.i, delta);
        ASSERT_EQ(k, j);
        j = delta;
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, SwapMethod) {
    using T = TypeParam;
    boxed<T, typename funcs<T>::methodType> x{magic<T>(), {}, magic<T>()};
    T j = 0;
    for (T delta : powersOfTwo<T>()) {
        T k = x.i.Swap(delta);
        ASSERT_EQ(x.i.Load(), delta);
        ASSERT_EQ(k, j);
        j = delta;
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, Add) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    T j = 0;
    for (T delta : powersOfTwo<T>()) {
        T k = funcs<T>::Add(&x.i, delta);
        j = T(j + delta);
        ASSERT_EQ(x.i, j);
        ASSERT_EQ(k, j);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, AddMethod) {
    using T = TypeParam;
    boxed<T, typename funcs<T>::methodType> x{magic<T>(), {}, magic<T>()};
    T j = 0;
    for (T delta : powersOfTwo<T>()) {
        T k = x.i.Add(delta);
        j = T(j + delta);
        ASSERT_EQ(x.i.Load(), j);
        ASSERT_EQ(k, j);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, And) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), T(~T(0)), magic<T>()};
    T j = T(~T(0));
    for (T mask : powersOfTwo<T>()) {
        T k = funcs<T>::And(&x.i, T(~mask));
        j = T(j & ~mask);
        ASSERT_EQ(x.i, j);
        ASSERT_EQ(k, T(j | mask)) << "the old value";
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, AndMethod) {
    using T = TypeParam;
    boxed<T, typename funcs<T>::methodType> x{magic<T>(), {}, magic<T>()};
    x.i.Store(T(~T(0)));
    T j = T(~T(0));
    for (T mask : powersOfTwo<T>()) {
        T k = x.i.And(T(~mask));
        j = T(j & ~mask);
        ASSERT_EQ(x.i.Load(), j);
        ASSERT_EQ(k, T(j | mask));
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, Or) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    T j = 0;
    for (T mask : powersOfTwo<T>()) {
        T k = funcs<T>::Or(&x.i, mask);
        ASSERT_EQ(k, j) << "the old value";
        j = T(j | mask);
        ASSERT_EQ(x.i, j);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, OrMethod) {
    using T = TypeParam;
    boxed<T, typename funcs<T>::methodType> x{magic<T>(), {}, magic<T>()};
    T j = 0;
    for (T mask : powersOfTwo<T>()) {
        T k = x.i.Or(mask);
        ASSERT_EQ(k, j);
        j = T(j | mask);
        ASSERT_EQ(x.i.Load(), j);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, CompareAndSwap) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    for (T val : powersOfTwo<T>()) {
        x.i = val;
        ASSERT_TRUE(funcs<T>::CompareAndSwap(&x.i, val, T(val + 1))) << "should have swapped " << val;
        ASSERT_EQ(x.i, T(val + 1)) << "wrong x.i after swap";
        x.i = T(val + 1);
        ASSERT_FALSE(funcs<T>::CompareAndSwap(&x.i, val, T(val + 2))) << "should not have swapped " << val;
        ASSERT_EQ(x.i, T(val + 1)) << "wrong x.i after swap";
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, CompareAndSwapMethod) {
    using T = TypeParam;
    boxed<T, typename funcs<T>::methodType> x{magic<T>(), {}, magic<T>()};
    for (T val : powersOfTwo<T>()) {
        x.i.Store(val);
        ASSERT_TRUE(x.i.CompareAndSwap(val, T(val + 1))) << "should have swapped " << val;
        ASSERT_EQ(x.i.Load(), T(val + 1));
        x.i.Store(T(val + 1));
        ASSERT_FALSE(x.i.CompareAndSwap(val, T(val + 2))) << "should not have swapped " << val;
        ASSERT_EQ(x.i.Load(), T(val + 1));
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, Load) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    for (T delta : powersOfTwo<T>()) {
        T k = funcs<T>::Load(&x.i);
        ASSERT_EQ(k, x.i);
        x.i = T(x.i + delta);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

TYPED_TEST(GoAtomic, Store) {
    using T = TypeParam;
    boxed<T, T> x{magic<T>(), 0, magic<T>()};
    T v = 0;
    for (T delta : powersOfTwo<T>()) {
        funcs<T>::Store(&x.i, v);
        ASSERT_EQ(x.i, v);
        v = T(v + delta);
    }
    ASSERT_EQ(x.before, magic<T>());
    ASSERT_EQ(x.after, magic<T>());
}

// Tests of correct behavior, without contention. (Does the function work?)
// Go's TestHammer32 and TestHammer64: fibers add to a counter at once; the
// sum is exact.
TYPED_TEST(GoAtomic, Hammer) {
    using T = TypeParam;
    constexpr int p = 4, n = 100000;
    typename funcs<T>::methodType m;
    T plain = 0;
    sync::WaitGroup wg;
    for (int i = 0; i < p; i++) {
        wg.Go([&] {
            for (int k = 0; k < n; k++) {
                m.Add(1);
                funcs<T>::Add(&plain, 1);
                // A CompareAndSwap loop, as Go's hammerCompareAndSwap does.
                for (;;) {
                    T v = m.Load();
                    if (m.CompareAndSwap(v, T(v + 1))) break;
                }
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(m.Load(), T(2 * p * n));
    EXPECT_EQ(funcs<T>::Load(&plain), T(p * n));
}

TEST(GoAtomic, Bool) {
    atomic::Bool b;
    EXPECT_FALSE(b.Load());
    b.Store(true);
    EXPECT_TRUE(b.Load());
    EXPECT_TRUE(b.Swap(false));
    EXPECT_FALSE(b.CompareAndSwap(true, true));
    EXPECT_TRUE(b.CompareAndSwap(false, true));
    EXPECT_TRUE(b.Load());
}

struct item final : object {
    int v;
    explicit item(int x) : v(x) {}
};

TEST(GoAtomic, SwapPointerMethod) {
    atomic::Pointer<item> x;
    ptr<item> j;
    for (int delta = 1; delta < 1 << 20; delta += delta) {
        auto k = make_ptr<item>(delta);
        auto old = x.Swap(k);
        ASSERT_EQ(x.Load(), k);
        ASSERT_EQ(old, j);
        j = k;
    }
}

TEST(GoAtomic, CompareAndSwapPointerMethod) {
    atomic::Pointer<item> x;
    auto q = make_ptr<item>(0);
    for (int val = 1; val < 1 << 20; val += val) {
        auto p = make_ptr<item>(val);
        x.Store(p);
        ASSERT_TRUE(x.CompareAndSwap(p, q)) << "should have swapped";
        ASSERT_EQ(x.Load(), q);
        ASSERT_FALSE(x.CompareAndSwap(p, nullptr)) << "should not have swapped";
        ASSERT_EQ(x.Load(), q);
    }
    EXPECT_TRUE(x.CompareAndSwap(q, nullptr));
    EXPECT_FALSE(x.Load());
}

TEST(GoAtomic, LoadStorePointerMethod) {
    atomic::Pointer<item> x;
    EXPECT_FALSE(x.Load());
    for (int delta = 1; delta < 1 << 20; delta += delta) {
        auto p = make_ptr<item>(delta);
        x.Store(p);
        ASSERT_EQ(x.Load(), p);
        ASSERT_EQ(x.Load()->v, delta);
    }
}

// Pointers swapped among fibers keep their reference counts: none is freed
// early or leaked.
TEST(GoAtomic, PointerConcurrent) {
    atomic::Pointer<item> x;
    x.Store(make_ptr<item>(0));
    sync::WaitGroup wg;
    for (int i = 0; i < 4; i++) {
        wg.Go([&x, i] {
            for (int k = 0; k < 20000; k++) {
                auto p = x.Load();
                ASSERT_TRUE(p);
                x.CompareAndSwap(p, make_ptr<item>(i));
                x.Swap(make_ptr<item>(k));
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(x.Load()->use_count(), 2u);  // x's and the Load's
}

// ---------------------------------------------------------------------------
// value_test.go

TEST(GoAtomic, Value) {
    atomic::Value<int> v;
    EXPECT_EQ(v.Load(), 0) << "initial Value is not nil";
    v.Store(42);
    EXPECT_EQ(v.Load(), 42);
    v.Store(84);
    EXPECT_EQ(v.Load(), 84);
}

TEST(GoAtomic, ValueLarge) {
    atomic::Value<std::string> v;
    v.Store("foo");
    EXPECT_EQ(v.Load(), "foo");
    v.Store("barbaz");
    EXPECT_EQ(v.Load(), "barbaz");
}

TEST(GoAtomic, ValueConcurrent) {
    std::vector<std::vector<int64>> tests = {{1, 2, 3, 4}, {int64(1) << 40, int64(2) << 50}};
    for (const auto& test : tests) {
        atomic::Value<int64> v;
        auto done = make_chan<bool>(4);
        for (int p = 0; p < 4; p++) {
            go([&v, &test, done] {
                for (int i = 0; i < 10000; i++) {
                    v.Store(test[std::size_t(i) % test.size()]);
                    int64 x = v.Load();
                    bool found = false;
                    for (int64 t : test) found = found || t == x;
                    if (!found) {
                        ADD_FAILURE() << "loaded bogus value " << x;
                        break;
                    }
                }
                done.send(true);
            });
        }
        for (int p = 0; p < 4; p++) done.recv();
    }
}

TEST(GoAtomic, Value_Swap) {
    atomic::Value<int> v;
    EXPECT_EQ(v.Swap(1), 0);
    EXPECT_EQ(v.Swap(2), 1);
    EXPECT_EQ(v.Load(), 2);
}

TEST(GoAtomic, ValueSwapConcurrent) {
    atomic::Value<uint64> v;
    uint64 count = 0;
    sync::WaitGroup g;
    sync::Mutex mu;
    constexpr int m = 10000, n = 4;
    for (int i = 0; i < n; i++) {
        g.Go([&, i] {
            uint64 c = 0;
            for (int j = i; j < m; j += n) c += v.Swap(uint64(j));
            std::lock_guard l(mu);
            count += c;
        });
    }
    g.Wait();
    uint64 want = uint64(m * (m - 1) / 2), got = count + v.Load();
    EXPECT_EQ(got, want) << "sum from 0 to m-1";
}

TEST(GoAtomic, Value_CompareAndSwap) {
    atomic::Value<int> v;
    EXPECT_TRUE(v.CompareAndSwap(0, 1)) << "never stored counts as the zero value";
    EXPECT_FALSE(v.CompareAndSwap(0, 2));
    EXPECT_TRUE(v.CompareAndSwap(1, 3));
    EXPECT_EQ(v.Load(), 3);
}

TEST(GoAtomic, ValueCompareAndSwapConcurrent) {
    atomic::Value<int> v;
    sync::WaitGroup w;
    v.Store(0);
    constexpr int m = 1000, n = 100;
    for (int i = 0; i < n; i++) {
        w.Go([&, i] {
            for (int j = i; j < m; j += n) {
                while (!v.CompareAndSwap(j, j + 1)) {
                    runtime::Gosched();
                }
            }
        });
    }
    w.Wait();
    EXPECT_EQ(v.Load(), m);
}

}  // namespace
