// linea - a value held in storage of its own, copied and compared as the value itself, so that a type can hold values
// of a type that holds it, as a node holds a group of nodes.
#pragma once

#include <memory>
#include <utility>

namespace linea {

// A value of type T held in storage of its own, which copies by copying the value and compares by comparing the
// values, as C++26's std::indirect (P3019). Where Indirect<T> is declared, T may be incomplete, so that a type can
// hold, through an Indirect, a type that holds it in turn. Reading, copying, comparing or destroying the value needs T
// to be complete where it happens.
//
// Moving an Indirect hands over its storage and leaves the source valueless, as std::indirect does. A valueless
// Indirect can be assigned to, copied, compared and destroyed, and is equal only to another valueless one, but holds no
// value to read.
template <class T> class Indirect {
  public:
    explicit Indirect(T value) : value_(std::make_unique<T>(std::move(value))) {}
    Indirect(const Indirect &other) : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    Indirect(Indirect &&) noexcept = default;
    // Copies the value before giving up the old one, so that a copy that throws leaves this Indirect as it was.
    Indirect &operator=(const Indirect &other) { return *this = Indirect(other); }
    Indirect &operator=(Indirect &&) noexcept = default;
    ~Indirect() = default;

    [[nodiscard]] T &operator*() { return *value_; }
    [[nodiscard]] const T &operator*() const { return *value_; }
    [[nodiscard]] T *operator->() { return value_.get(); }
    [[nodiscard]] const T *operator->() const { return value_.get(); }

    // Whether the two hold equal values, or are both valueless.
    friend bool operator==(const Indirect &a, const Indirect &b) {
        if (!a.value_ || !b.value_)
            return !a.value_ && !b.value_;
        return *a.value_ == *b.value_;
    }

  private:
    std::unique_ptr<T> value_;
};

} // namespace linea
