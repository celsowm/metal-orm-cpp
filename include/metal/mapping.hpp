#pragma once

#include <cstddef>
#include <string_view>

namespace metal::mapping {

template <std::size_t Capacity>
struct fixed_text {
    char data[Capacity]{};

    template <std::size_t N>
    consteval fixed_text(const char (&text)[N]) {
        static_assert(N <= Capacity, "MetalORM mapping string is too long");
        for (std::size_t i = 0; i < N; ++i) data[i] = text[i];
    }

    constexpr std::string_view view() const noexcept {
        std::size_t n = 0;
        while (n < Capacity && data[n] != '\0') ++n;
        return {data, n};
    }
};

struct table {
    fixed_text<96> name;

    template <std::size_t N>
    consteval table(const char (&value)[N]) : name(value) {}
};

struct column {
    fixed_text<96> name;

    template <std::size_t N>
    consteval column(const char (&value)[N]) : name(value) {}
};

struct primary_key_t {};
struct generated_t {};
struct ignore_t {};

inline constexpr primary_key_t primary_key{};
inline constexpr generated_t generated{};
inline constexpr ignore_t ignore{};

struct many_to_many {
    fixed_text<96> pivot_table;
    fixed_text<96> pivot_root_fk;
    fixed_text<96> pivot_target_fk;
    fixed_text<96> root_key{"id"};
    fixed_text<96> target_key{"id"};

    template <std::size_t A, std::size_t B, std::size_t C>
    consteval many_to_many(
        const char (&pivot)[A],
        const char (&root_fk)[B],
        const char (&target_fk)[C])
        : pivot_table(pivot), pivot_root_fk(root_fk), pivot_target_fk(target_fk) {}

    template <std::size_t A, std::size_t B, std::size_t C, std::size_t D, std::size_t E>
    consteval many_to_many(
        const char (&pivot)[A],
        const char (&root_fk)[B],
        const char (&target_fk)[C],
        const char (&root)[D],
        const char (&target)[E])
        : pivot_table(pivot), pivot_root_fk(root_fk), pivot_target_fk(target_fk),
          root_key(root), target_key(target) {}
};

} // namespace metal::mapping
