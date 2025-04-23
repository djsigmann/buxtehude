#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <initializer_list>
#include <string>
#include <tuple>
#include <concepts>

namespace buxtehude
{

using nlohmann::json, nlohmann::json_pointer;

using Predicate = std::function<bool(const json& j)>;
using ValidationPair = std::pair<json_pointer<std::string>, Predicate>;
using ValidationSeries = std::initializer_list<ValidationPair>;

bool ValidateJSON(const json& j, const ValidationSeries& tests);

namespace predicates
{

struct Compare
{
    Compare(const json& j) : cmp(j) {}
    bool operator()(const json& j) { return j == cmp; }
    const json cmp;
};

struct Matches
{
    Matches(const std::initializer_list<json>& j) : cmp(j) {}
    bool operator()(const json& j)
    {
        for (const json& x : cmp) if (j == x) return true;
        return false;
    }
    const std::vector<json> cmp;
};

struct Inverse
{
    Inverse(const Predicate& p) : pred(p) {}
    bool operator()(const json& j) { return !pred(j); }
    Predicate pred;
};

enum class EqualityType
{
    EQUAL, LESS, GREATER, LESS_EQ, GREATER_EQ
};

template<EqualityType eq, auto cmp>
inline const Predicate IntegralCompare = [] (const json& j) -> bool
{
    switch (eq) {
    case EqualityType::EQUAL: return j == cmp;
    case EqualityType::LESS: return j < cmp;
    case EqualityType::GREATER: return j > cmp;
    case EqualityType::LESS_EQ: return j <= cmp;
    case EqualityType::GREATER_EQ: return j >= cmp;
    default: return false;
    }
};

template<auto cmp>
inline const Predicate GreaterEq = IntegralCompare<EqualityType::GREATER_EQ, cmp>;

inline const Predicate Exists = nullptr;
inline const Predicate NotEmpty = [] (const json& j) { return j.is_string() && j != ""; };
inline const Predicate IsBool = [] (const json& j) { return j.is_boolean(); };

}

}
