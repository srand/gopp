// The container/list package: a doubly linked list, ported from Go
// 1.27.3, as a template on the element type.
//
//     list::List<int> l;
//     auto e4 = l.PushBack(4);
//     l.PushFront(1);
//     l.InsertBefore(3, e4);
//     for (auto e = l.Front(); e; e = e->Next()) fmt::Println("{}", e->Value);
//
// Elements are reference counted: one that is removed from its list stays
// valid while held, as in Go, and Next and Prev of it are then nil.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/ptr.hpp>

#include <utility>
#endif

namespace gopp::list {

template <typename T>
class List;

namespace detail {
struct node {
    node* next = nullptr;
    node* prev = nullptr;
};
}  // namespace detail

// list.Element: an element of a List.
template <typename T>
class Element final : public object, private detail::node {
public:
    // The value stored with this element.
    T Value{};

    explicit Element(T v) : Value(std::move(v)) {}

    // The next list element or nil.
    ptr<Element> Next() const {
        if (list_ && next != &list_->root_) return ptr<Element>(static_cast<Element*>(next));
        return nullptr;
    }
    // The previous list element or nil.
    ptr<Element> Prev() const {
        if (list_ && prev != &list_->root_) return ptr<Element>(static_cast<Element*>(prev));
        return nullptr;
    }

private:
    friend class List<T>;
    List<T>* list_ = nullptr;  // the list this element belongs to
};

// list.List: a doubly linked list. The zero List is an empty list ready to
// use. It holds a reference to each of its elements.
template <typename T>
class List {
public:
    List() noexcept { Init(); }
    List(const List&) = delete;
    List& operator=(const List&) = delete;
    ~List() { clear(); }

    // Clears the list.
    List& Init() {
        clear();
        root_.next = &root_;
        root_.prev = &root_;
        len_ = 0;
        return *this;
    }

    // The number of elements.
    isize Len() const noexcept { return len_; }

    // The first element or nil.
    ptr<Element<T>> Front() const { return len_ == 0 ? nullptr : ptr<Element<T>>(elem(root_.next)); }
    // The last element or nil.
    ptr<Element<T>> Back() const { return len_ == 0 ? nullptr : ptr<Element<T>>(elem(root_.prev)); }

    // Removes e from l if e is an element of l, and returns its value.
    T Remove(const ptr<Element<T>>& e) {
        if (e->list_ == this) remove(e.get());
        return e->Value;
    }

    // Inserts a new element with value v at the front, and returns it.
    ptr<Element<T>> PushFront(T v) { return insertValue(std::move(v), &root_); }
    // Inserts a new element with value v at the back, and returns it.
    ptr<Element<T>> PushBack(T v) { return insertValue(std::move(v), root_.prev); }
    // Inserts a new element with value v right before mark, and returns
    // it; nil if mark isn't an element of l.
    ptr<Element<T>> InsertBefore(T v, const ptr<Element<T>>& mark) {
        if (mark->list_ != this) return nullptr;
        return insertValue(std::move(v), mark->prev);
    }
    // Inserts a new element with value v right after mark, and returns it;
    // nil if mark isn't an element of l.
    ptr<Element<T>> InsertAfter(T v, const ptr<Element<T>>& mark) {
        if (mark->list_ != this) return nullptr;
        return insertValue(std::move(v), mark.get());
    }

    // Moves e to the front; if e isn't an element of l, nothing happens.
    void MoveToFront(const ptr<Element<T>>& e) {
        if (e->list_ != this || root_.next == e.get()) return;
        move(e.get(), &root_);
    }
    // Moves e to the back.
    void MoveToBack(const ptr<Element<T>>& e) {
        if (e->list_ != this || root_.prev == e.get()) return;
        move(e.get(), root_.prev);
    }
    // Moves e right before mark; nothing happens if either isn't an
    // element of l, or e is mark.
    void MoveBefore(const ptr<Element<T>>& e, const ptr<Element<T>>& mark) {
        if (e->list_ != this || e == mark || mark->list_ != this) return;
        move(e.get(), mark->prev);
    }
    // Moves e right after mark.
    void MoveAfter(const ptr<Element<T>>& e, const ptr<Element<T>>& mark) {
        if (e->list_ != this || e == mark || mark->list_ != this) return;
        move(e.get(), mark.get());
    }

    // Inserts copies of the values of other at the back. other may be l.
    void PushBackList(const List& other) {
        isize i = other.Len();
        for (auto e = other.Front(); i > 0; i--, e = e->Next()) insertValue(e->Value, root_.prev);
    }
    // Inserts copies of the values of other at the front. other may be l.
    void PushFrontList(const List& other) {
        isize i = other.Len();
        for (auto e = other.Back(); i > 0; i--, e = e->Prev()) insertValue(e->Value, &root_);
    }

private:
    friend class Element<T>;

    static Element<T>* elem(detail::node* n) { return static_cast<Element<T>*>(n); }

    // insert inserts e after at, increments l.len, and returns e.
    ptr<Element<T>> insertValue(T v, detail::node* at) {
        auto* e = new Element<T>(std::move(v));
        e->addRef();  // the list's reference
        e->prev = at;
        e->next = at->next;
        e->prev->next = e;
        e->next->prev = e;
        e->list_ = this;
        len_++;
        return ptr<Element<T>>(e);
    }

    // remove removes e from its list, decrements l.len.
    void remove(Element<T>* e) {
        e->prev->next = e->next;
        e->next->prev = e->prev;
        e->next = nullptr;
        e->prev = nullptr;
        e->list_ = nullptr;
        len_--;
        e->releaseRef();  // the list's reference
    }

    // move moves e to next to at.
    void move(Element<T>* e, detail::node* at) {
        if (e == at) return;
        e->prev->next = e->next;
        e->next->prev = e->prev;
        e->prev = at;
        e->next = at->next;
        e->prev->next = e;
        e->next->prev = e;
    }

    void clear() {
        if (!root_.next) return;
        for (detail::node* n = root_.next; n != &root_;) {
            detail::node* next = n->next;
            auto* e = elem(n);
            e->next = e->prev = nullptr;
            e->list_ = nullptr;
            e->releaseRef();
            n = next;
        }
        root_.next = root_.prev = &root_;
        len_ = 0;
    }

    detail::node root_;  // sentinel list element, only &root, root.prev, and root.next are used
    isize len_ = 0;      // current list length excluding (this) sentinel element
};

}  // namespace gopp::list
