/*
 * SlopFin - text subtitles.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "subtitles.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace slopfin::subtitles
{
namespace
{
std::mutex g_mutex;
std::vector<Cue> g_cues;

/* "01:02:03,456" or "01:02:03.456"; returns a negative value when malformed. */
double parse_time(std::string_view text)
{
    int parts[4] = {};
    int field = 0;
    int digits = 0;
    for (const char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            parts[field] = parts[field] * 10 + (c - '0');
            ++digits;
        }
        else if ((c == ':' || c == ',' || c == '.') && field < 3)
        {
            ++field;
        }
        else if (c != ' ' && c != '\t')
        {
            return -1.0;
        }
    }
    if (field != 3 || digits == 0)
        return -1.0;
    return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2] + parts[3] / 1000.0;
}

/*
 * Strip markup a television cannot render sensibly: HTML-style tags such as
 * <i> and <font>, and ASS override blocks such as {\an8}, which survive the
 * server's conversion to SubRip.
 */
std::string clean(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (c == '<')
        {
            const std::size_t close = text.find('>', i);
            if (close != std::string_view::npos)
            {
                i = close;
                continue;
            }
        }
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '\\')
        {
            const std::size_t close = text.find('}', i);
            if (close != std::string_view::npos)
            {
                i = close;
                continue;
            }
        }
        if (c == '\\' && i + 1 < text.size() && (text[i + 1] == 'N' || text[i + 1] == 'n'))
        {
            out += '\n';
            ++i;
            continue;
        }
        if (c == '&')
        {
            static constexpr std::pair<std::string_view, char> kEntities[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}};
            bool matched = false;
            for (const auto &[entity, value] : kEntities)
            {
                if (text.substr(i, entity.size()) == entity)
                {
                    out += value;
                    i += entity.size() - 1;
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
        }
        if (c != '\r')
            out += c;
    }
    /* Trim trailing blank lines and spaces left behind by removed tags. */
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    std::size_t first = 0;
    while (first < out.size() && (out[first] == '\n' || out[first] == ' '))
        ++first;
    return out.substr(first);
}
} // namespace

std::vector<Cue> parse_srt(std::string_view text)
{
    std::vector<Cue> cues;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        text.remove_prefix(3);

    std::size_t position = 0;
    Cue cue;
    bool in_cue = false;
    std::string body;
    const auto finish = [&]()
    {
        if (in_cue)
        {
            cue.text = clean(body);
            if (!cue.text.empty() && cue.end > cue.start)
                cues.push_back(cue);
        }
        in_cue = false;
        body.clear();
    };

    while (position <= text.size())
    {
        std::size_t line_end = text.find('\n', position);
        if (line_end == std::string_view::npos)
            line_end = text.size();
        std::string_view line = text.substr(position, line_end - position);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        position = line_end + 1;

        const std::size_t arrow = line.find("-->");
        if (arrow != std::string_view::npos)
        {
            finish();
            std::string_view right = line.substr(arrow + 3);
            /* Position hints such as "X1:.." may follow the end time. */
            while (!right.empty() && right.front() == ' ')
                right.remove_prefix(1);
            const std::size_t space = right.find(' ');
            if (space != std::string_view::npos)
                right = right.substr(0, space);
            cue.start = parse_time(line.substr(0, arrow));
            cue.end = parse_time(right);
            in_cue = cue.start >= 0.0 && cue.end >= 0.0;
            continue;
        }
        if (line.empty())
        {
            finish();
            continue;
        }
        if (in_cue)
        {
            if (!body.empty())
                body += '\n';
            body += line;
        }
        if (line_end == text.size())
            break;
    }
    finish();

    std::stable_sort(cues.begin(), cues.end(),
                     [](const Cue &a, const Cue &b) { return a.start < b.start; });
    return cues;
}

std::string active(const std::vector<Cue> &cues, double seconds)
{
    /* The first cue that starts after now; everything showing is before it. */
    const auto after = std::upper_bound(cues.begin(), cues.end(), seconds,
                                        [](double t, const Cue &cue) { return t < cue.start; });
    std::string out;
    /* Overlapping cues are rare but real, so look back a handful rather than one. */
    auto it = after;
    for (int looked = 0; it != cues.begin() && looked < 8; ++looked)
    {
        --it;
        if (seconds < it->end)
            out = out.empty() ? it->text : it->text + "\n" + out;
    }
    return out;
}

void set(std::vector<Cue> cues)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_cues = std::move(cues);
}

void clear()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_cues.clear();
}

std::string current(double seconds)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return active(g_cues, seconds);
}

std::size_t count()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return g_cues.size();
}

bool loaded()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return !g_cues.empty();
}

} // namespace slopfin::subtitles
