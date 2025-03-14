#pragma once

#include <cstddef>
#include <span>
#include <ranges>

namespace buxtehude
{

// Making up for lack of C++23/26 std::span, std::ranges & std::expected features
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

template<typename Lambda> requires std::invocable<Lambda>
struct scoped_guard
{
    Lambda lambda;
    scoped_guard(Lambda&& lambda) : lambda(std::move(lambda)) {}
    ~scoped_guard() { lambda(); }
};

struct ok_t {};

template<typename R, typename E>
struct result_internal
{
    union {
        R value;
        E err;
    };
    const bool is_err;

    result_internal(const R& value) : value(value), is_err(false) {}
    result_internal(const E& err) : err(err), is_err(true) {}
    ~result_internal() { if (!is_err) value.~R(); }
};

template<typename E>
struct result_internal<void, E>
{
    E err;
    const bool is_err;

    result_internal(ok_t) : is_err(false) {}
    result_internal(const E& err) : err(err), is_err(true) {}
};

template<typename R, typename E> requires std::is_empty_v<E>
struct result_internal<R, E>
{
    union { R value; };
    const bool is_err;

    result_internal(const R& value) : value(value), is_err(false) {}
    result_internal(const E&) : is_err(true) {}
    ~result_internal() { if (!is_err) value.~R(); }
};

template<typename E> requires std::is_empty_v<E>
struct result_internal<void, E>
{
    const bool is_err;

    result_internal(ok_t) : is_err(false) {}
    result_internal(const E&) : is_err(true) {}
};

template<typename R, typename E>
struct [[nodiscard]] result
{
private:
    result_internal<R, E> members;
public:
    result() = delete;

    // Success/value initialisation
    template<typename T = R> requires (std::is_void_v<T> && std::is_same_v<T, R>)
    result(ok_t) : members { ok_t {} } {}

    template<typename T = R> requires (!std::is_void_v<T> && std::is_same_v<T, R>)
    result(const T& value) : members { value } {}

    // Error initialisation
    template<typename T = E> requires (!std::is_empty_v<T> && std::is_same_v<T, E>)
    result(const T& err) : members { err } {}

    template<typename T = E> requires (std::is_empty_v<T> && std::is_same_v<T, E>)
    result(const T& err) : members { err } {}

    bool is_error() const { return members.is_err; }
    bool is_ok() const { return !members.is_err; }

    template<typename Callable>
    const result& if_ok(Callable&& cb) const
    {
        if constexpr (std::is_void_v<R>) {
            if (!members.is_err) cb();
        } else {
            if (!members.is_err) cb(members.value);
        }
        return *this;
    }

    template<typename Callable>
    const result& if_err(Callable&& cb) const
    {
        if constexpr (std::is_empty_v<E>) {
            if (members.is_err) cb(E {});
        } else {
            if (members.is_err) cb(members.err);
        }
        return *this;
    }

    template<typename T = R> requires (!std::is_void_v<T>)
    const T& get_or(T&& alternative) const
    {
        return members.is_err ? alternative : members.value;
    }

    template<typename T = R> requires (!std::is_same_v<T, void>)
    const T& get_unchecked() const { return members.value; }

    template<typename T = R> requires (!std::is_same_v<T, void>)
    T& get_mut_unchecked() { return members.value; }

    auto get_error() const
    {
        if constexpr (std::is_empty_v<E>) return E {};
        else return members.err;
    }
};

template<typename E>
using error = result<void, E>;

} // namespace tb

} // namespace buxtehude
