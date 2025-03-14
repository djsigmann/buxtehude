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

template<typename R, typename E>
struct [[nodiscard]] result
{
private:
    union {
        R value;
        E err;
    };
    bool is_err;
public:
    result(const R& value) : value(value), is_err(false) {}
    result(const E& err) : err(err), is_err(true) {}

    bool is_error() const { return is_err; }
    bool is_ok() const { return !is_err; }

    template<typename Callable>
    const result& if_ok(Callable&& cb) const
    {
        if (!is_err) cb(value);
        return *this;
    }

    template<typename Callable>
    const result& if_err(Callable&& cb) const
    {
        if (is_err) cb(err);
        return *this;
    }

    R& get_or(R&& alternative) const
    {
        return is_err ? alternative : value;
    }

    R& get() { return value; }
    const E& get_error() { return err; }
    ~result() { if (!is_err) value.~R(); }
};

struct ok_t {};

template<typename E>
struct [[nodiscard]] result<void, E>
{
private:
    E err;
    bool is_err = false;
public:
    result() = delete;
    result(ok_t) : is_err(false) {}
    result(const E& err) : err(err), is_err(true) {}

    bool is_error() const { return is_err; }
    bool is_ok() const { return !is_err; }

    template<typename Callable>
    const result& if_ok(Callable&& cb) const
    {
        if (!is_err) cb();
        return *this;
    }

    template<typename Callable>
    const result& if_err(Callable&& cb) const
    {
        if (is_err) cb(err);
        return *this;
    }
};

template<typename E>
using error = result<void, E>;

} // namespace tb

} // namespace buxtehude
