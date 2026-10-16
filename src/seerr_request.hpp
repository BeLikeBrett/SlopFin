/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_SEERR_REQUEST_HPP
#define SLOPFIN_SEERR_REQUEST_HPP

#include "json.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace slopfin::seerr::request
{
inline bool permitted(std::uint32_t permissions, std::uint32_t flags) noexcept
{
    return (permissions & (2u | flags)) != 0;
}
inline bool advanced(std::uint32_t permissions) noexcept
{
    return permitted(permissions, 16u | 8192u);
}
inline bool can_request(std::uint32_t permissions, bool tv, bool is4k) noexcept
{
    return permitted(permissions,
                     is4k ? (1024u | (tv ? 4096u : 2048u)) : (32u | (tv ? 524288u : 262144u)));
}
struct Season
{
    int number = 0;
    int episodes = 0;
    std::string name;
    int status = 1;
    int status4k = 1;
    bool requested = false;
    bool requested4k = false;
    bool selectable(bool is4k) const noexcept
    {
        const int state = is4k ? status4k : status;
        return episodes > 0 && (state <= 1 || state == 6) && !(is4k ? requested4k : requested);
    }
};
struct Profile
{
    int id = 0;
    std::string name;
};
struct Server
{
    int id = 0;
    std::string name;
    bool is4k = false;
    bool is_default = false;
    int default_profile = 0;
    std::string directory;
    std::vector<Profile> profiles;
};
struct Options
{
    bool loaded = false;
    bool advanced = false;
    bool allow4k = false;
    bool partial = true;
    int status = 1;
    int status4k = 1;
    std::string jellyfin_id;
    std::string jellyfin_id4k;
    std::vector<Season> seasons;
    std::vector<Server> servers;
    std::string error;
    std::string profile_error;
};
inline std::vector<Season> read_seasons(const json::Value &detail)
{
    std::vector<Season> seasons;
    const auto *list = detail.find("seasons");
    const auto *info = detail.find("mediaInfo");
    if (!list)
        return seasons;
    for (std::size_t i = 0; i < std::min<std::size_t>(list->size(), 500); ++i)
    {
        const auto *entry = list->at(i);
        if (!entry || entry->num("episodeCount") < 1)
            continue;
        Season season;
        season.number = static_cast<int>(entry->num("seasonNumber", -1));
        if (season.number < 0)
            continue;
        season.episodes = static_cast<int>(entry->num("episodeCount"));
        season.name = std::string(entry->str("name"));
        if (info)
        {
            if (const auto *states = info->find("seasons"))
                for (std::size_t n = 0; n < states->size(); ++n)
                    if (const auto *s = states->at(n);
                        s && s->num("seasonNumber", -1) == season.number)
                    {
                        season.status = static_cast<int>(s->num("status", 1));
                        season.status4k = static_cast<int>(s->num("status4k", 1));
                    }
            if (const auto *requests = info->find("requests"))
                for (std::size_t n = 0; n < requests->size(); ++n)
                {
                    const auto *r = requests->at(n);
                    if (!r || (r->num("status") != 1 && r->num("status") != 2))
                        continue;
                    if (const auto *requested = r->find("seasons"))
                        for (std::size_t j = 0; j < requested->size(); ++j)
                            if (const auto *s = requested->at(j);
                                s && s->num("seasonNumber", -1) == season.number &&
                                s->num("status") != 3)
                            {
                                if (r->flag("is4k"))
                                    season.requested4k = true;
                                else
                                    season.requested = true;
                            }
                }
        }
        seasons.push_back(std::move(season));
    }
    std::sort(seasons.begin(), seasons.end(),
              [](const Season &a, const Season &b) { return a.number < b.number; });
    return seasons;
}
inline std::string payload(const std::string &type, int id, bool is4k, const Options &options,
                           const std::vector<int> &selected, int server_index, int profile_index)
{
    if (!options.loaded || !options.error.empty() || id <= 0 || (type != "movie" && type != "tv"))
        return {};
    std::string body = "{\"mediaType\":\"" + type + "\",\"mediaId\":" + std::to_string(id) +
                       ",\"is4k\":" + (is4k ? "true" : "false");
    if (is4k && !options.allow4k)
        return {};
    if (type == "tv")
    {
        std::string numbers;
        for (const auto &season : options.seasons)
            if (season.selectable(is4k) &&
                std::find(selected.begin(), selected.end(), season.number) != selected.end())
            {
                if (!numbers.empty())
                    numbers += ',';
                numbers += std::to_string(season.number);
            }
        if (numbers.empty())
            return {};
        body += options.partial ? ",\"seasons\":[" + numbers + "]" : ",\"seasons\":\"all\"";
    }
    if (options.advanced && server_index >= 0 &&
        server_index < static_cast<int>(options.servers.size()))
    {
        const auto &server = options.servers[static_cast<std::size_t>(server_index)];
        if (server.is4k != is4k)
            return {};
        body += ",\"serverId\":" + std::to_string(server.id);
        if (profile_index >= 0 && profile_index < static_cast<int>(server.profiles.size()))
            body += ",\"profileId\":" +
                    std::to_string(server.profiles[static_cast<std::size_t>(profile_index)].id);
        if (!server.directory.empty())
            body += ",\"rootFolder\":\"" + json::escape(server.directory) + "\"";
    }
    return body + '}';
}
} // namespace slopfin::seerr::request
#endif
