// The container/heap package: heap operations on any type that implements
// heap::Interface, ported from Go 1.27.3. A heap is a tree whose every
// node is the minimum (by Less) of its subtree, kept in an array: the
// basis of priority queues.
//
//     struct IntHeap : heap::Interface<int> {
//         std::vector<int> v;
//         isize Len() override { return isize(v.size()); }
//         bool Less(isize i, isize j) override { return v[i] < v[j]; }
//         void Swap(isize i, isize j) override { std::swap(v[i], v[j]); }
//         void Push(int x) override { v.push_back(x); }
//         int Pop() override { int x = v.back(); v.pop_back(); return x; }
//     };
//     IntHeap h;
//     heap::Push(h, 3);
//     int smallest = heap::Pop(h);
//
// Go's Interface takes and returns any; go++'s is a template on the element
// type.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/sort.hpp>

#include <utility>
#endif

namespace gopp::heap {

// heap.Interface: a sort::Interface whose elements can be pushed and
// popped at the end. Push adds x as element Len(); Pop removes and returns
// element Len()-1. Use heap::Push and heap::Pop, not these.
template <typename T>
struct Interface : sort::Interface {
    virtual void Push(T x) = 0;
    virtual T Pop() = 0;
};

namespace detail {

inline void up(sort::Interface& h, isize j) {
    for (;;) {
        isize i = (j - 1) / 2;  // parent
        if (i == j || !h.Less(j, i)) break;
        h.Swap(i, j);
        j = i;
    }
}

inline bool down(sort::Interface& h, isize i0, isize n) {
    isize i = i0;
    for (;;) {
        isize j1 = 2 * i + 1;
        if (j1 >= n || j1 < 0) break;  // j1 < 0 after int overflow
        isize j = j1;                   // left child
        if (isize j2 = j1 + 1; j2 < n && h.Less(j2, j1)) j = j2;  // = 2*i + 2, right child
        if (!h.Less(j, i)) break;
        h.Swap(i, j);
        i = j;
    }
    return i > i0;
}

}  // namespace detail

// heap.Init: establishes the heap invariants. It may be called whenever
// they may have been invalidated. O(n).
template <typename T>
void Init(Interface<T>& h) {
    // heapify
    isize n = h.Len();
    for (isize i = n / 2 - 1; i >= 0; i--) detail::down(h, i, n);
}

// heap.Push: pushes x onto the heap. O(log n).
template <typename T, typename U>
void Push(Interface<T>& h, U&& x) {
    h.Push(T(std::forward<U>(x)));
    detail::up(h, h.Len() - 1);
}

// heap.Pop: removes and returns the minimum element (by Less). O(log n).
template <typename T>
T Pop(Interface<T>& h) {
    isize n = h.Len() - 1;
    h.Swap(0, n);
    detail::down(h, 0, n);
    return h.Pop();
}

// heap.Remove: removes and returns the element at index i. O(log n).
template <typename T>
T Remove(Interface<T>& h, isize i) {
    isize n = h.Len() - 1;
    if (n != i) {
        h.Swap(i, n);
        if (!detail::down(h, i, n)) detail::up(h, i);
    }
    return h.Pop();
}

// heap.Fix: re-establishes the heap ordering after the element at index i
// has changed its value. Cheaper than Remove and Push. O(log n).
template <typename T>
void Fix(Interface<T>& h, isize i) {
    if (!detail::down(h, i, h.Len())) detail::up(h, i);
}

}  // namespace gopp::heap
