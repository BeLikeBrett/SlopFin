/*
 * SlopFin - read-only JSON parser.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Parses into a tree of owned nodes. Jellyfin responses are modest and read
 * once, so clarity beats a zero-copy design here.
 */

#ifndef SLOPFIN_JSON_HPP
#define SLOPFIN_JSON_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace slopfin::json
{

class Value;
using ValuePtr = std::unique_ptr<Value>;

enum class Type : std::uint8_t
{
    null = 0,
    boolean,
    number,
    string,
    array,
    object,
};

class Value final
{
  public:
    Type type = Type::null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<ValuePtr> elements;                       /* array */
    std::vector<std::pair<std::string, ValuePtr>> fields; /* object */

    /* Object lookup; returns nullptr when absent. Case-sensitive. */
    [[nodiscard]] const Value *find(std::string_view key) const noexcept;

    [[nodiscard]] std::string_view string_or(std::string_view fallback) const noexcept;
    [[nodiscard]] double number_or(double fallback) const noexcept;
    [[nodiscard]] bool bool_or(bool fallback) const noexcept;

    /* Convenience: object field as a string, empty when missing. */
    [[nodiscard]] std::string_view str(std::string_view key) const noexcept;
    [[nodiscard]] double num(std::string_view key, double fallback = 0.0) const noexcept;
    [[nodiscard]] bool flag(std::string_view key, bool fallback = false) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return elements.size();
    }
    [[nodiscard]] const Value *at(std::size_t index) const noexcept;
};

/* Returns nullptr on malformed input. */
ValuePtr parse(std::string_view text) noexcept;

/* Escapes a string for embedding in a JSON document. */
std::string escape(std::string_view value) noexcept;

} // namespace slopfin::json

#endif
