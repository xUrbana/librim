#pragma once

#include <variant>

namespace librim
{

/**
 * @struct unexpected
 * @brief Represents a logical failure state container for `librim::expected`.
 *
 * Modeled verbatim after `std::unexpected` from C++23.
 *
 * @tparam E Type representing the error constraint.
 */
template <typename E> struct unexpected
{
    E val; ///< Inner explicit error state.

    /**
     * @brief Instantiates the failure envelope natively.
     */
    explicit unexpected(E e) : val(std::move(e))
    {
    }
};

/**
 * @brief Deduction guide inferring the template Type natively.
 */
template <typename E> unexpected(E) -> unexpected<E>;

/**
 * @class expected
 * @brief C++23 `std::expected` shim utilized extensively for clean error handling.
 *
 * Heavily deployed across `librim` networking boundaries to fundamentally 
 * avoid the extreme performance consequences of traditional C++ exception throwing 
 * in highly concurrent ASIO event loop pipelines.
 *
 * @tparam T Logical success type.
 * @tparam E Explicit failure enumeration or object.
 */
template <typename T, typename E> class expected
{
    std::variant<T, unexpected<E>> var;

  public:
    /**
     * @brief Instantiates a conclusively successful state.
     */
    expected(T val) : var(std::move(val))
    {
    }

    /**
     * @brief Instantiates an overtly negative/failed state logically.
     */
    template <typename Err> expected(unexpected<Err> err) : var(unexpected<E>(std::move(err.val)))
    {
    }

    /**
     * @return `true` if the state represents logical active success.
     */
    bool has_value() const
    {
        return var.index() == 0;
    }
    
    /**
     * @brief Explicit operator allowing syntax like `if(res) { ... }`.
     */
    explicit operator bool() const
    {
        return has_value();
    }

    /**
     * @brief Synchronously extracts the active value physically. Unsafe if error.
     */
    T &value()
    {
        return std::get<0>(var);
    }
    const T &value() const
    {
        return std::get<0>(var);
    }

    /**
     * @brief Synchronously extracts the encapsulated logical error code physically.
     */
    E &error()
    {
        return std::get<1>(var).val;
    }
    const E &error() const
    {
        return std::get<1>(var).val;
    }
};

/**
 * @class expected<void, E>
 * @brief Explicit specialization for void return models yielding purely success/error bools natively.
 */
template <typename E> class expected<void, E>
{
    std::variant<std::monostate, unexpected<E>> var;

  public:
    /**
     * @brief Success explicitly defines zero return parameters structurally.
     */
    expected() : var(std::monostate{})
    {
    }

    /**
     * @brief Failed explicit state dynamically bound.
     */
    template <typename Err> expected(unexpected<Err> err) : var(unexpected<E>(std::move(err.val)))
    {
    }

    /**
     * @return Logical check boolean.
     */
    bool has_value() const
    {
        return var.index() == 0;
    }
    explicit operator bool() const
    {
        return has_value();
    }

    /**
     * @brief Explicitly zero-effect routine fulfilling template signature compatibility natively.
     */
    void value() const
    {
    }

    E &error()
    {
        return std::get<1>(var).val;
    }
    const E &error() const
    {
        return std::get<1>(var).val;
    }
};

} // namespace librim
