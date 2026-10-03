/*
 * SlopFin - read-only JSON parser.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "json.hpp"

#include <cstdlib>

namespace
{
using slopfin::json::Type;
using slopfin::json::Value;
using slopfin::json::ValuePtr;

class Parser final
{
  public:
    explicit Parser(std::string_view text) noexcept : text_{text}
    {
    }

    ValuePtr parse() noexcept
    {
        skip_space();
        ValuePtr root = parse_value(0);
        if (root == nullptr)
            return nullptr;
        skip_space();
        return root;
    }

  private:
    static constexpr int kMaxDepth = 64;

    std::string_view text_;
    std::size_t cursor_ = 0;

    void skip_space() noexcept
    {
        while (cursor_ < text_.size())
        {
            const char ch = text_[cursor_];
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
                break;
            ++cursor_;
        }
    }

    bool consume(char expected) noexcept
    {
        if (cursor_ < text_.size() && text_[cursor_] == expected)
        {
            ++cursor_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word) noexcept
    {
        if (text_.compare(cursor_, word.size(), word) != 0)
            return false;
        cursor_ += word.size();
        return true;
    }

    /* Appends one codepoint as UTF-8. */
    static void append_utf8(std::string &out, std::uint32_t codepoint) noexcept
    {
        if (codepoint < 0x80u)
        {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800u)
        {
            out.push_back(static_cast<char>(0xc0u | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
        else if (codepoint < 0x10000u)
        {
            out.push_back(static_cast<char>(0xe0u | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0u | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
        }
    }

    std::uint32_t parse_hex4() noexcept
    {
        std::uint32_t value = 0;
        for (int i = 0; i < 4 && cursor_ < text_.size(); ++i)
        {
            const char ch = text_[cursor_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
                value |= static_cast<std::uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
        }
        return value;
    }

    bool parse_string(std::string &out) noexcept
    {
        if (!consume('"'))
            return false;
        while (cursor_ < text_.size())
        {
            const char ch = text_[cursor_++];
            if (ch == '"')
                return true;
            if (ch != '\\')
            {
                out.push_back(ch);
                continue;
            }
            if (cursor_ >= text_.size())
                return false;
            const char escape = text_[cursor_++];
            switch (escape)
            {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case '/':
                out.push_back('/');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            case 'u':
            {
                std::uint32_t codepoint = parse_hex4();
                /* Recombine a surrogate pair when the low half follows. */
                if (codepoint >= 0xd800u && codepoint <= 0xdbffu &&
                    text_.compare(cursor_, 2, "\\u") == 0)
                {
                    cursor_ += 2;
                    const std::uint32_t low = parse_hex4();
                    if (low >= 0xdc00u && low <= 0xdfffu)
                        codepoint = 0x10000u + ((codepoint - 0xd800u) << 10) + (low - 0xdc00u);
                }
                append_utf8(out, codepoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    ValuePtr parse_value(int depth) noexcept
    {
        if (depth > kMaxDepth || cursor_ >= text_.size())
            return nullptr;
        skip_space();
        if (cursor_ >= text_.size())
            return nullptr;

        auto node = std::make_unique<Value>();
        const char ch = text_[cursor_];
        if (ch == '{')
        {
            ++cursor_;
            node->type = Type::object;
            skip_space();
            if (consume('}'))
                return node;
            while (true)
            {
                skip_space();
                std::string key;
                if (!parse_string(key))
                    return nullptr;
                skip_space();
                if (!consume(':'))
                    return nullptr;
                ValuePtr child = parse_value(depth + 1);
                if (child == nullptr)
                    return nullptr;
                node->fields.emplace_back(std::move(key), std::move(child));
                skip_space();
                if (consume(','))
                    continue;
                if (consume('}'))
                    return node;
                return nullptr;
            }
        }
        if (ch == '[')
        {
            ++cursor_;
            node->type = Type::array;
            skip_space();
            if (consume(']'))
                return node;
            while (true)
            {
                ValuePtr child = parse_value(depth + 1);
                if (child == nullptr)
                    return nullptr;
                node->elements.push_back(std::move(child));
                skip_space();
                if (consume(','))
                    continue;
                if (consume(']'))
                    return node;
                return nullptr;
            }
        }
        if (ch == '"')
        {
            node->type = Type::string;
            return parse_string(node->text) ? std::move(node) : nullptr;
        }
        if (literal("true"))
        {
            node->type = Type::boolean;
            node->boolean = true;
            return node;
        }
        if (literal("false"))
        {
            node->type = Type::boolean;
            node->boolean = false;
            return node;
        }
        if (literal("null"))
        {
            node->type = Type::null;
            return node;
        }

        const std::size_t start = cursor_;
        if (consume('-'))
            ;
        while (cursor_ < text_.size())
        {
            const char digit = text_[cursor_];
            if ((digit >= '0' && digit <= '9') || digit == '.' || digit == 'e' || digit == 'E' ||
                digit == '+' || digit == '-')
                ++cursor_;
            else
                break;
        }
        if (cursor_ == start)
            return nullptr;
        node->type = Type::number;
        node->number =
            std::strtod(std::string(text_.substr(start, cursor_ - start)).c_str(), nullptr);
        return node;
    }
};
} // namespace

namespace slopfin::json
{

const Value *Value::find(std::string_view key) const noexcept
{
    for (const auto &entry : fields)
    {
        if (entry.first == key)
            return entry.second.get();
    }
    return nullptr;
}

const Value *Value::at(std::size_t index) const noexcept
{
    return index < elements.size() ? elements[index].get() : nullptr;
}

std::string_view Value::string_or(std::string_view fallback) const noexcept
{
    return type == Type::string ? std::string_view{text} : fallback;
}

double Value::number_or(double fallback) const noexcept
{
    return type == Type::number ? number : fallback;
}

bool Value::bool_or(bool fallback) const noexcept
{
    return type == Type::boolean ? boolean : fallback;
}

std::string_view Value::str(std::string_view key) const noexcept
{
    const Value *child = find(key);
    return child == nullptr ? std::string_view{} : child->string_or({});
}

double Value::num(std::string_view key, double fallback) const noexcept
{
    const Value *child = find(key);
    return child == nullptr ? fallback : child->number_or(fallback);
}

bool Value::flag(std::string_view key, bool fallback) const noexcept
{
    const Value *child = find(key);
    return child == nullptr ? fallback : child->bool_or(fallback);
}

ValuePtr parse(std::string_view text) noexcept
{
    Parser parser{text};
    return parser.parse();
}

std::string escape(std::string_view value) noexcept
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const char raw : value)
    {
        switch (raw)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(raw) < 0x20u)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                out.append("\\u00");
                out.push_back(kHex[(raw >> 4) & 0x0f]);
                out.push_back(kHex[raw & 0x0f]);
            }
            else
            {
                out.push_back(raw);
            }
            break;
        }
    }
    return out;
}

} // namespace slopfin::json
