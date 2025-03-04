#pragma once

#include <cstddef>
#include <span>
#include <ranges>

namespace buxtehude
{

// Making up for lack of C++23/26 span & range features
namespace tb
{

template<typename T, size_t N>
constexpr auto make_span(T (&&array)[N]) { return std::span<T>(array, N); }

template<typename T>
struct range_to_dummy {};

template<typename T>
struct ptr_vec_dummy {};

template<typename T>
constexpr auto range_to() { return range_to_dummy<T> {}; }

template<typename T>
constexpr auto ptr_vec() { return ptr_vec_dummy<T> {}; }

template<std::ranges::view View, typename T>
constexpr auto operator|(View&& view, range_to_dummy<T>&&)
{
    return T { view.begin(), view.end() };
}

template<typename Range, typename T>
constexpr auto operator|(Range&& range, ptr_vec_dummy<T>&&)
{
    return range | std::views::transform([] (auto&& obj) -> T* {
        if constexpr (std::is_same_v<std::unique_ptr<T>,
                      typename std::ranges::range_value_t<Range>>) {
            return obj.get();
        } else return &obj;
    }) | range_to<std::vector<T*>>();
}

} // namespace tb

} // namespace buxtehude
