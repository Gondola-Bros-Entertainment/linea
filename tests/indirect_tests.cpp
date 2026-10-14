#include "linea/Indirect.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace linea;

namespace {

// A list that holds the rest of itself through an Indirect, declared while the list is still incomplete.
struct List {
    int head{};
    std::optional<Indirect<List>> tail{};
    friend bool operator==(const List &, const List &) = default;
};

} // namespace

TEST_CASE("An Indirect copies its value, so a copy changes apart from the original") {
    const Indirect<std::vector<int>> original(std::vector<int>{1, 2, 3});
    Indirect<std::vector<int>> copy = original;
    CHECK(&*copy != &*original);
    copy->push_back(4);
    CHECK(*original == std::vector<int>{1, 2, 3});
    // Assigning copies the value too.
    Indirect<std::vector<int>> assigned(std::vector<int>{});
    assigned = copy;
    CHECK(&*assigned != &*copy);
    assigned->clear();
    CHECK(*copy == std::vector<int>{1, 2, 3, 4});
    // Assigning an Indirect to itself keeps its value.
    const Indirect<std::vector<int>> &same = copy;
    copy = same;
    CHECK(*copy == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("Indirects compare their values, not where they hold them") {
    const Indirect<std::string> a(std::string("linea"));
    const Indirect<std::string> b(std::string("linea"));
    CHECK(&*a != &*b);
    CHECK(a == b);
    CHECK(a != Indirect<std::string>(std::string("line")));
}

TEST_CASE("A moved-from Indirect is valueless: it copies and compares as valueless, and takes a value by assignment") {
    Indirect<std::string> source(std::string("linea"));
    const std::string *held = &*source;
    const Indirect<std::string> moved = std::move(source);
    // Moving hands over the storage rather than copying the value.
    CHECK(&*moved == held);
    const Indirect<std::string> copy = source;
    CHECK(copy == source);
    CHECK(copy != moved);
    CHECK(moved != source);
    source = moved;
    CHECK(source == moved);
    CHECK(*source == "linea");
}

TEST_CASE("An Indirect can hold a type that holds it, and copies and compares the whole of it") {
    const List list{1, Indirect<List>(List{2, Indirect<List>(List{.head = 3})})};
    List copy = list;
    CHECK(copy == list);
    List &second = **copy.tail;
    List &third = **second.tail;
    third.head = 30;
    CHECK(copy != list);
    CHECK((**(**list.tail).tail).head == 3);
}
