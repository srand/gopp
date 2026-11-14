// The HTML entity tables (entity.cpp).
#pragma once

#include <gopp/base.hpp>

#include <cstddef>
#include <iterator>
#include <string_view>

namespace gopp::html::detail {

// All entities that do not end with ';' are 6 or fewer bytes long.
inline constexpr std::size_t longestEntityWithoutSemicolon = 6;

struct entity {
    std::string_view name;
    rune r;
};
struct entity2 {
    std::string_view name;
    rune r[2];
};

extern const entity entities[];
extern const std::size_t nEntities;
extern const entity2 entities2[];
extern const std::size_t nEntities2;

}  // namespace gopp::html::detail
