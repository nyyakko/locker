#pragma once

#include <functional>
#include <ranges>
#include <unordered_map>

template <class ... Keys>
struct TupleHasher
{
    auto operator()(auto const& keys) const
    {
        constexpr auto tupleSize = std::tuple_size_v<std::tuple<Keys...>>;

        auto fnUnpackAndHash = [] <std::size_t ... Index> (auto&& keys, std::index_sequence<Index ...>) constexpr
        {
            return (std::hash<
                std::decay_t<std::tuple_element_t<Index, std::tuple<Keys...>>>
            >{}(std::get<Index>(keys)) ^ ...);
        };

        return fnUnpackAndHash(keys, std::make_index_sequence<tupleSize>{});
    }
};

template <class Return, class ... Arguments> class Memoizer;

template<class Return, class ... Arguments>
class Memoizer<Return(Arguments...)>
{
public:
    auto& operator=(auto&& functor)
    {
        this->function = std::forward<decltype(functor)>(functor);
        return *this;
    }

    template<typename... Args>
    auto operator()(Args&&... args)
    {
        const auto key = std::make_tuple(args...);

        if (cache.find(key) == cache.end())
        {
            cache[key] = std::invoke(function, std::forward<Args>(args)...);
        }

        return cache[key];
    }

private:
    std::function<Return(Arguments...)> function;
    std::unordered_map<std::tuple<Arguments...>, Return, TupleHasher<Arguments...>> cache;
};
