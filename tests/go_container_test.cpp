// Ported from Go 1.27.3's container/heap/heap_test.go and
// container/list/list_test.go, with Go's test names. Go's lists hold any
// values; TestList's list here holds std::any. checkListPointers checks
// Next and Prev only, as the links are private.
#include <gopp/container/heap.hpp>
#include <gopp/container/list.hpp>

#include <gtest/gtest.h>

#include <any>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace gopp;

namespace {

// ---------------------------------------------------------------------------
// heap

struct myHeap final : heap::Interface<int> {
    std::vector<int> v;
    bool Less(isize i, isize j) override { return v[std::size_t(i)] < v[std::size_t(j)]; }
    void Swap(isize i, isize j) override { std::swap(v[std::size_t(i)], v[std::size_t(j)]); }
    isize Len() override { return isize(v.size()); }
    int Pop() override {
        int x = v.back();
        v.pop_back();
        return x;
    }
    void Push(int x) override { v.push_back(x); }

    void verify(isize i) {
        isize n = Len();
        isize j1 = 2 * i + 1;
        isize j2 = 2 * i + 2;
        if (j1 < n) {
            if (Less(j1, i)) {
                ADD_FAILURE() << "heap invariant invalidated [" << i << "] = " << v[std::size_t(i)] << " > [" << j1 << "] = " << v[std::size_t(j1)];
                return;
            }
            verify(j1);
        }
        if (j2 < n) {
            if (Less(j2, i)) {
                ADD_FAILURE() << "heap invariant invalidated [" << i << "] = " << v[std::size_t(i)] << " > [" << j1 << "] = " << v[std::size_t(j2)];
                return;
            }
            verify(j2);
        }
    }
};

TEST(GoContainerHeap, Init0) {
    myHeap h;
    for (int i = 20; i > 0; i--) h.Push(0);  // all elements are the same
    heap::Init(h);
    h.verify(0);
    for (int i = 1; h.Len() > 0; i++) {
        int x = heap::Pop(h);
        h.verify(0);
        EXPECT_EQ(x, 0) << i << ".th pop";
    }
}

TEST(GoContainerHeap, Init1) {
    myHeap h;
    for (int i = 20; i > 0; i--) h.Push(i);  // all elements are different
    heap::Init(h);
    h.verify(0);
    for (int i = 1; h.Len() > 0; i++) {
        int x = heap::Pop(h);
        h.verify(0);
        EXPECT_EQ(x, i) << i << ".th pop";
    }
}

TEST(GoContainerHeap, Test) {
    myHeap h;
    h.verify(0);
    for (int i = 20; i > 10; i--) h.Push(i);
    heap::Init(h);
    h.verify(0);
    for (int i = 10; i > 0; i--) {
        heap::Push(h, i);
        h.verify(0);
    }
    for (int i = 1; h.Len() > 0; i++) {
        int x = heap::Pop(h);
        if (i < 20) heap::Push(h, 20 + i);
        h.verify(0);
        EXPECT_EQ(x, i) << i << ".th pop";
    }
}

TEST(GoContainerHeap, Remove0) {
    myHeap h;
    for (int i = 0; i < 10; i++) h.Push(i);
    h.verify(0);
    while (h.Len() > 0) {
        isize i = h.Len() - 1;
        int x = heap::Remove(h, i);
        EXPECT_EQ(x, i) << "Remove(" << i << ")";
        h.verify(0);
    }
}

TEST(GoContainerHeap, Remove1) {
    myHeap h;
    for (int i = 0; i < 10; i++) h.Push(i);
    h.verify(0);
    for (int i = 0; h.Len() > 0; i++) {
        int x = heap::Remove(h, 0);
        EXPECT_EQ(x, i) << "Remove(0)";
        h.verify(0);
    }
}

TEST(GoContainerHeap, Remove2) {
    constexpr int N = 10;
    myHeap h;
    for (int i = 0; i < N; i++) h.Push(i);
    h.verify(0);
    std::map<int, bool> m;
    while (h.Len() > 0) {
        m[heap::Remove(h, (h.Len() - 1) / 2)] = true;
        h.verify(0);
    }
    EXPECT_EQ(m.size(), std::size_t(N));
    for (int i = 0; i < int(m.size()); i++) EXPECT_TRUE(m[i]) << "m[" << i << "] doesn't exist";
}

TEST(GoContainerHeap, Fix) {
    myHeap h;
    h.verify(0);
    for (int i = 200; i > 0; i -= 10) heap::Push(h, i);
    h.verify(0);
    ASSERT_EQ(h.v[0], 10) << "Expected head to be 10";
    h.v[0] = 210;
    heap::Fix(h, 0);
    h.verify(0);
    std::mt19937 rnd(1);
    for (int i = 100; i > 0; i--) {
        isize elem = isize(rnd() % std::size_t(h.Len()));
        if (i & 1) {
            h.v[std::size_t(elem)] /= 2;
        } else {
            h.v[std::size_t(elem)] *= 2;
        }
        heap::Fix(h, elem);
        h.verify(0);
    }
}

// ---------------------------------------------------------------------------
// list

template <typename T>
bool checkListLen(const list::List<T>& l, isize len) {
    if (isize n = l.Len(); n != len) {
        ADD_FAILURE() << "l.Len() = " << n << ", want " << len;
        return false;
    }
    return true;
}

template <typename T>
void checkListPointers(const list::List<T>& l, const std::vector<ptr<list::Element<T>>>& es) {
    if (!checkListLen(l, isize(es.size()))) return;
    if (es.empty()) {
        EXPECT_FALSE(l.Front());
        EXPECT_FALSE(l.Back());
        return;
    }
    EXPECT_EQ(l.Front(), es.front());
    EXPECT_EQ(l.Back(), es.back());
    for (std::size_t i = 0; i < es.size(); i++) {
        ptr<list::Element<T>> Prev = i > 0 ? es[i - 1] : nullptr;
        EXPECT_EQ(es[i]->Prev(), Prev) << "elt[" << i << "].Prev()";
        ptr<list::Element<T>> Next = i + 1 < es.size() ? es[i + 1] : nullptr;
        EXPECT_EQ(es[i]->Next(), Next) << "elt[" << i << "].Next()";
    }
}

TEST(GoContainerList, List) {
    list::List<std::any> l;
    using E = ptr<list::Element<std::any>>;
    checkListPointers<std::any>(l, {});

    // Single element list
    E e = l.PushFront(std::string("a"));
    checkListPointers<std::any>(l, {e});
    l.MoveToFront(e);
    checkListPointers<std::any>(l, {e});
    l.MoveToBack(e);
    checkListPointers<std::any>(l, {e});
    l.Remove(e);
    checkListPointers<std::any>(l, {});

    // Bigger list
    E e2 = l.PushFront(2);
    E e1 = l.PushFront(1);
    E e3 = l.PushBack(3);
    E e4 = l.PushBack(std::string("banana"));
    checkListPointers<std::any>(l, {e1, e2, e3, e4});

    l.Remove(e2);
    checkListPointers<std::any>(l, {e1, e3, e4});

    l.MoveToFront(e3);  // move from middle
    checkListPointers<std::any>(l, {e3, e1, e4});

    l.MoveToFront(e1);
    l.MoveToBack(e3);  // move from middle
    checkListPointers<std::any>(l, {e1, e4, e3});

    l.MoveToFront(e3);  // move from back
    checkListPointers<std::any>(l, {e3, e1, e4});
    l.MoveToFront(e3);  // should be no-op
    checkListPointers<std::any>(l, {e3, e1, e4});

    l.MoveToBack(e3);  // move from front
    checkListPointers<std::any>(l, {e1, e4, e3});
    l.MoveToBack(e3);  // should be no-op
    checkListPointers<std::any>(l, {e1, e4, e3});

    e2 = l.InsertBefore(2, e1);  // insert before front
    checkListPointers<std::any>(l, {e2, e1, e4, e3});
    l.Remove(e2);
    e2 = l.InsertBefore(2, e4);  // insert before middle
    checkListPointers<std::any>(l, {e1, e2, e4, e3});
    l.Remove(e2);
    e2 = l.InsertBefore(2, e3);  // insert before back
    checkListPointers<std::any>(l, {e1, e4, e2, e3});
    l.Remove(e2);

    e2 = l.InsertAfter(2, e1);  // insert after front
    checkListPointers<std::any>(l, {e1, e2, e4, e3});
    l.Remove(e2);
    e2 = l.InsertAfter(2, e4);  // insert after middle
    checkListPointers<std::any>(l, {e1, e4, e2, e3});
    l.Remove(e2);
    e2 = l.InsertAfter(2, e3);  // insert after back
    checkListPointers<std::any>(l, {e1, e4, e3, e2});
    l.Remove(e2);

    // Check standard iteration.
    int sum = 0;
    for (auto it = l.Front(); it; it = it->Next()) {
        if (auto* i = std::any_cast<int>(&it->Value)) sum += *i;
    }
    EXPECT_EQ(sum, 4) << "sum over l";

    // Clear all elements by iterating
    E next;
    for (auto it = l.Front(); it; it = next) {
        next = it->Next();
        l.Remove(it);
    }
    checkListPointers<std::any>(l, {});
}

void checkList(const list::List<int>& l, const std::vector<int>& es) {
    if (!checkListLen(l, isize(es.size()))) return;
    std::size_t i = 0;
    for (auto e = l.Front(); e; e = e->Next()) {
        EXPECT_EQ(e->Value, es[i]) << "elt[" << i << "].Value";
        i++;
    }
}

TEST(GoContainerList, Extending) {
    list::List<int> l1, l2;
    l1.PushBack(1);
    l1.PushBack(2);
    l1.PushBack(3);
    l2.PushBack(4);
    l2.PushBack(5);

    {
        list::List<int> l3;
        l3.PushBackList(l1);
        checkList(l3, {1, 2, 3});
        l3.PushBackList(l2);
        checkList(l3, {1, 2, 3, 4, 5});
    }
    {
        list::List<int> l3;
        l3.PushFrontList(l2);
        checkList(l3, {4, 5});
        l3.PushFrontList(l1);
        checkList(l3, {1, 2, 3, 4, 5});
    }
    checkList(l1, {1, 2, 3});
    checkList(l2, {4, 5});
    {
        list::List<int> l3;
        l3.PushBackList(l1);
        checkList(l3, {1, 2, 3});
        l3.PushBackList(l3);
        checkList(l3, {1, 2, 3, 1, 2, 3});
    }
    {
        list::List<int> l3;
        l3.PushFrontList(l1);
        checkList(l3, {1, 2, 3});
        l3.PushFrontList(l3);
        checkList(l3, {1, 2, 3, 1, 2, 3});
    }
    {
        list::List<int> l3;
        l1.PushBackList(l3);
        checkList(l1, {1, 2, 3});
        l1.PushFrontList(l3);
        checkList(l1, {1, 2, 3});
    }
}

TEST(GoContainerList, Remove) {
    list::List<int> l;
    auto e1 = l.PushBack(1);
    auto e2 = l.PushBack(2);
    checkListPointers<int>(l, {e1, e2});
    auto e = l.Front();
    l.Remove(e);
    checkListPointers<int>(l, {e2});
    l.Remove(e);
    checkListPointers<int>(l, {e2});
}

TEST(GoContainerList, Issue4103) {
    list::List<int> l1;
    l1.PushBack(1);
    l1.PushBack(2);
    list::List<int> l2;
    l2.PushBack(3);
    l2.PushBack(4);
    auto e = l1.Front();
    l2.Remove(e);  // l2 should not change because e is not an element of l2
    EXPECT_EQ(l2.Len(), 2);
    l1.InsertBefore(8, e);
    EXPECT_EQ(l1.Len(), 3);
}

TEST(GoContainerList, Issue6349) {
    list::List<int> l;
    l.PushBack(1);
    l.PushBack(2);
    auto e = l.Front();
    l.Remove(e);
    EXPECT_EQ(e->Value, 1);
    EXPECT_FALSE(e->Next());
    EXPECT_FALSE(e->Prev());
}

TEST(GoContainerList, Move) {
    list::List<int> l;
    auto e1 = l.PushBack(1);
    auto e2 = l.PushBack(2);
    auto e3 = l.PushBack(3);
    auto e4 = l.PushBack(4);

    l.MoveAfter(e3, e3);
    checkListPointers<int>(l, {e1, e2, e3, e4});
    l.MoveBefore(e2, e2);
    checkListPointers<int>(l, {e1, e2, e3, e4});

    l.MoveAfter(e3, e2);
    checkListPointers<int>(l, {e1, e2, e3, e4});
    l.MoveBefore(e2, e3);
    checkListPointers<int>(l, {e1, e2, e3, e4});

    l.MoveBefore(e2, e4);
    checkListPointers<int>(l, {e1, e3, e2, e4});
    std::swap(e2, e3);

    l.MoveBefore(e4, e1);
    checkListPointers<int>(l, {e4, e1, e2, e3});
    std::tie(e1, e2, e3, e4) = std::make_tuple(e4, e1, e2, e3);

    l.MoveAfter(e4, e1);
    checkListPointers<int>(l, {e1, e4, e2, e3});
    std::tie(e2, e3, e4) = std::make_tuple(e4, e2, e3);

    l.MoveAfter(e2, e3);
    checkListPointers<int>(l, {e1, e3, e2, e4});
}

// Test PushFront, PushBack, PushFrontList, PushBackList with uninitialized
// List.
TEST(GoContainerList, ZeroList) {
    list::List<int> l1;
    l1.PushFront(1);
    checkList(l1, {1});
    list::List<int> l2;
    l2.PushBack(1);
    checkList(l2, {1});
    list::List<int> l3;
    l3.PushFrontList(l1);
    checkList(l3, {1});
    list::List<int> l4;
    l4.PushBackList(l2);
    checkList(l4, {1});
}

// Test that a list l is not modified when calling InsertBefore with a mark
// that is not an element of l.
TEST(GoContainerList, InsertBeforeUnknownMark) {
    list::List<int> l;
    l.PushBack(1);
    l.PushBack(2);
    l.PushBack(3);
    l.InsertBefore(1, make_ptr<list::Element<int>>(0));
    checkList(l, {1, 2, 3});
}

// Test that a list l is not modified when calling InsertAfter with a mark
// that is not an element of l.
TEST(GoContainerList, InsertAfterUnknownMark) {
    list::List<int> l;
    l.PushBack(1);
    l.PushBack(2);
    l.PushBack(3);
    l.InsertAfter(1, make_ptr<list::Element<int>>(0));
    checkList(l, {1, 2, 3});
}

// Test that a list l is not modified when calling MoveAfter or MoveBefore
// with a mark that is not an element of l.
TEST(GoContainerList, MoveUnknownMark) {
    list::List<int> l1;
    auto e1 = l1.PushBack(1);
    list::List<int> l2;
    auto e2 = l2.PushBack(2);
    l1.MoveAfter(e1, e2);
    checkList(l1, {1});
    checkList(l2, {2});
    l1.MoveBefore(e1, e2);
    checkList(l1, {1});
    checkList(l2, {2});
}

// go++: elements outlive the list they were in, as Go's do.
TEST(ContainerList, ElementOutlivesList) {
    ptr<list::Element<std::string>> e;
    {
        list::List<std::string> l;
        e = l.PushBack("kept");
        l.PushBack("dropped");
    }
    EXPECT_EQ(e->Value, "kept");
    EXPECT_FALSE(e->Next());
    EXPECT_EQ(e->use_count(), 1u);
}

}  // namespace
