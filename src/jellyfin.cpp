/*
 * SlopFin - Jellyfin API client.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "jellyfin.hpp"
#include "playback_url.hpp"
#include "hevc_base_layer.hpp"
#include "intro_metadata.hpp"

#include "config.hpp"
#include "http.hpp"
#include "json.hpp"
#include "trace.hpp"

#include <algorithm>
#include <cctype>

namespace
{
std::string g_error;

/* Jellyfin's scheme-style authorization header, with the token when present. */
std::vector<std::string> auth_headers() noexcept
{
    const slopfin::config::Settings &settings = slopfin::config::current();
    std::string value = "Authorization: MediaBrowser Client=\"SlopFin\", Device=\"PlayStation 5\"";
    value += ", DeviceId=\"" + settings.device_id + "\", Version=\"0.1\"";
    if (!settings.token.empty())
        value += ", Token=\"" + settings.token + "\"";
    return {value};
}

slopfin::http::Response request_get(const std::string &path) noexcept
{
    const slopfin::config::Settings &settings = slopfin::config::current();
    return slopfin::http::get(settings.host, settings.port, path, auth_headers());
}

slopfin::http::Response request_post(const std::string &path, const std::string &body) noexcept
{
    const slopfin::config::Settings &settings = slopfin::config::current();
    return slopfin::http::post(settings.host, settings.port, path, auth_headers(), body,
                               "application/json");
}

/* Pulls the fields the UI needs out of one BaseItemDto. */
slopfin::jellyfin::Item read_item(const slopfin::json::Value &node) noexcept
{
    slopfin::jellyfin::Item item;
    item.id = std::string{node.str("Id")};
    item.name = std::string{node.str("Name")};
    item.overview = std::string{node.str("Overview")};
    item.type = std::string{node.str("Type")};
    item.series_name = std::string{node.str("SeriesName")};
    item.production_year = static_cast<int>(node.num("ProductionYear", 0));
    item.index_number = static_cast<int>(node.num("IndexNumber", 0));
    item.parent_index_number = static_cast<int>(node.num("ParentIndexNumber", 0));
    item.child_count = static_cast<int>(node.num("ChildCount", 0.0));
    item.community_rating = node.num("CommunityRating", 0.0);
    item.critic_rating = node.num("CriticRating", 0.0);

    /* Ticks are 100 ns units; minutes is what the UI shows. */
    const double ticks = node.num("RunTimeTicks", 0.0);
    item.runtime_minutes = static_cast<int>(ticks / 600000000.0 + 0.5);

    if (const slopfin::json::Value *tags = node.find("ImageTags"); tags != nullptr)
    {
        item.image_tag = std::string{tags->str("Primary")};
        item.logo_tag = std::string{tags->str("Logo")};
    }
    if (const slopfin::json::Value *backdrops = node.find("BackdropImageTags");
        backdrops != nullptr && backdrops->size() > 0)
    {
        const slopfin::json::Value *first = backdrops->at(0);
        if (first != nullptr)
            item.backdrop_tag = std::string{first->string_or({})};
    }
    item.series_id = std::string{node.str("SeriesId")};
    item.series_primary_tag = std::string{node.str("SeriesPrimaryImageTag")};
    item.parent_backdrop_id = std::string{node.str("ParentBackdropItemId")};
    item.parent_logo_id = std::string{node.str("ParentLogoItemId")};
    item.parent_logo_tag = std::string{node.str("ParentLogoImageTag")};
    if (const slopfin::json::Value *parent = node.find("ParentBackdropImageTags");
        parent != nullptr && parent->size() > 0)
    {
        const slopfin::json::Value *first = parent->at(0);
        if (first != nullptr)
            item.parent_backdrop_tag = std::string{first->string_or({})};
    }

    item.official_rating = std::string{node.str("OfficialRating")};
    item.premiere_date = std::string{node.str("PremiereDate")};
    item.date_created = std::string{node.str("DateCreated")};
    item.status = std::string{node.str("Status")};
    item.end_date = std::string{node.str("EndDate")};
    item.season_id = std::string{node.str("SeasonId")};
    item.recursive_item_count = static_cast<int>(node.num("RecursiveItemCount", 0.0));
    /* Named chapters fill gaps when the server has no detected segments. */
    const auto chapter_markers =
        slopfin::intro::chapters(node.find("Chapters"), ticks / slopfin::intro::kTicksPerSecond);
    item.intro_start_seconds = chapter_markers.intro_start;
    item.intro_end_seconds = chapter_markers.intro_end;
    item.credits_start_seconds = chapter_markers.outro_start;
    if (const slopfin::json::Value *taglines = node.find("Taglines");
        taglines != nullptr && taglines->size() > 0)
        if (const slopfin::json::Value *first = taglines->at(0); first != nullptr)
            item.tagline = std::string{first->string_or({})};
    if (const slopfin::json::Value *studios = node.find("Studios"); studios != nullptr)
        for (std::size_t i = 0; i < studios->size() && i < 3; ++i)
            if (const slopfin::json::Value *studio = studios->at(i); studio != nullptr)
                item.studios +=
                    (item.studios.empty() ? "" : ", ") + std::string{studio->str("Name")};
    if (const slopfin::json::Value *people = node.find("People"); people != nullptr)
        for (std::size_t i = 0; i < people->size() && item.people.size() < 40; ++i)
            if (const slopfin::json::Value *entry = people->at(i); entry != nullptr)
                item.people.push_back(
                    {std::string{entry->str("Id")}, std::string{entry->str("Name")},
                     std::string{entry->str("Role")}, std::string{entry->str("Type")},
                     std::string{entry->str("PrimaryImageTag")}});

    if (const slopfin::json::Value *sources = node.find("MediaSources"); sources != nullptr)
    {
        for (std::size_t i = 0; i < sources->size(); ++i)
        {
            const slopfin::json::Value *entry = sources->at(i);
            if (entry == nullptr)
                continue;
            slopfin::jellyfin::MediaSource source;
            source.id = std::string{entry->str("Id")};
            source.name = std::string{entry->str("Name")};
            source.bitrate = static_cast<long long>(entry->num("Bitrate", 0.0));
            source.path = std::string{entry->str("Path")};
            source.container = std::string{entry->str("Container")};
            source.size_bytes = static_cast<long long>(entry->num("Size", 0.0));

            source.default_audio = static_cast<int>(entry->num("DefaultAudioStreamIndex", -1.0));
            source.default_subtitle =
                static_cast<int>(entry->num("DefaultSubtitleStreamIndex", -1.0));
            const auto read_track = [](const slopfin::json::Value &stream)
            {
                slopfin::jellyfin::Track track;
                track.index = static_cast<int>(stream.num("Index", -1.0));
                track.label = std::string{stream.str("DisplayTitle")};
                /*
                 * A remux names every stream after the release, and the server
                 * puts that in front of each DisplayTitle -- eleven rows of
                 * "The.Dark.Knight.2008.2160p.BluRay.REMUX..." with the
                 * language cut off the end. The stream's own Title is that
                 * prefix, so it is dropped.
                 */
                if (const std::string_view title = stream.str("Title");
                    !title.empty() && track.label.size() > title.size() + 3 &&
                    track.label.compare(0, title.size(), title) == 0 &&
                    track.label.compare(title.size(), 3, " - ") == 0)
                    track.label.erase(0, title.size() + 3);
                track.language = std::string{stream.str("Language")};
                track.codec = std::string{stream.str("Codec")};
                track.profile = std::string{stream.str("Profile")};
                track.channels = static_cast<int>(stream.num("Channels", 0.0));
                track.bitrate = static_cast<long long>(stream.num("BitRate", 0.0));
                track.is_default = stream.flag("IsDefault", false);
                track.is_forced = stream.flag("IsForced", false);
                track.is_text = stream.flag("IsTextSubtitleStream", false);
                track.is_external = stream.flag("IsExternal", false);
                return track;
            };
            if (const slopfin::json::Value *streams = entry->find("MediaStreams");
                streams != nullptr)
            {
                for (std::size_t s = 0; s < streams->size(); ++s)
                {
                    const slopfin::json::Value *stream = streams->at(s);
                    if (stream == nullptr)
                        continue;
                    const std::string_view kind = stream->str("Type");
                    if (kind == "Video" && source.width == 0)
                    {
                        source.video_codec = std::string{stream->str("Codec")};
                        source.width = static_cast<int>(stream->num("Width", 0.0));
                        source.video_profile = std::string{stream->str("Profile")};
                        source.video_level = stream->num("Level", 0.0);
                        source.video_bit_depth = static_cast<int>(stream->num("BitDepth", 0.0));
                        source.video_range = std::string{stream->str("VideoRangeType")};
                        source.video_dovi = std::string{stream->str("VideoDoViTitle")};
                        source.video_bitrate = static_cast<long long>(stream->num("BitRate", 0.0));
                        source.aspect_ratio = std::string{stream->str("AspectRatio")};
                        source.height = static_cast<int>(stream->num("Height", 0.0));
                        /* RealFrameRate is the coded rate; AverageFrameRate can
                           be a measured approximation of it. */
                        source.frame_rate = stream->num("RealFrameRate", 0.0);
                        if (source.frame_rate <= 0.0)
                            source.frame_rate = stream->num("AverageFrameRate", 0.0);
                    }
                    else if (kind == "Audio")
                    {
                        if (source.audio_codec.empty())
                        {
                            source.audio_codec = std::string{stream->str("Codec")};
                            source.audio_channels = static_cast<int>(stream->num("Channels", 0.0));
                        }
                        source.audio_tracks.push_back(read_track(*stream));
                    }
                    else if (kind == "Subtitle")
                    {
                        source.subtitle_tracks.push_back(read_track(*stream));
                        source.has_subtitles = true;
                        const std::string_view title = stream->str("DisplayTitle");
                        if (title.find("SDH") != std::string_view::npos ||
                            stream->flag("IsHearingImpaired", false))
                            source.subtitles_sdh = true;
                    }
                }
            }
            item.sources.push_back(std::move(source));
        }
    }
    if (const slopfin::json::Value *genres = node.find("Genres"); genres != nullptr)
    {
        for (std::size_t i = 0; i < genres->size() && i < 3; ++i)
        {
            const slopfin::json::Value *genre = genres->at(i);
            if (genre == nullptr)
                continue;
            if (!item.genres.empty())
                item.genres += ", ";
            item.genres += std::string{genre->string_or({})};
        }
    }

    if (const slopfin::json::Value *user = node.find("UserData"); user != nullptr)
    {
        item.played = user->flag("Played", false);
        item.played_percentage = user->num("PlayedPercentage", 0.0);
        item.resume_ticks = user->num("PlaybackPositionTicks", 0.0);
    }
    return item;
}

slopfin::intro::Markers media_markers(const std::string &item_id, double runtime) noexcept
{
    if (item_id.empty())
        return {};
    const slopfin::http::Response response = request_get("/MediaSegments/" + item_id);
    if (!response.ok())
        return {};
    const slopfin::json::ValuePtr root = slopfin::json::parse(response.body);
    return root == nullptr ? slopfin::intro::Markers{} : slopfin::intro::segments(*root, runtime);
}

std::vector<slopfin::jellyfin::Item> read_items(const std::string &body) noexcept
{
    std::vector<slopfin::jellyfin::Item> out;
    const slopfin::json::ValuePtr root = slopfin::json::parse(body);
    if (root == nullptr)
        return out;

    /* Some endpoints return a bare array, others wrap it in Items. */
    const slopfin::json::Value *list = root.get();
    if (const slopfin::json::Value *wrapped = root->find("Items"); wrapped != nullptr)
        list = wrapped;
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const slopfin::json::Value *node = list->at(i);
        if (node != nullptr)
            out.push_back(read_item(*node));
    }
    return out;
}
} // namespace

namespace slopfin::jellyfin
{

std::string MediaSource::quality_label() const noexcept
{
    /* Width as well as height: films cropped to a wide ratio are shorter
       than their tier (a 4K scope film is 3840x1600, 1080p scope 1920x800),
       and height alone called Interstellar's 4K copy "1440p". */
    if (width >= 3200 || height >= 2000)
        return "4K";
    if (width >= 2400 || height >= 1400)
        return "1440p";
    if (width >= 1700 || height >= 900)
        return "1080p";
    if (width >= 1200 || height >= 600)
        return "720p";
    if (height > 0)
        return std::to_string(height) + "p";
    return {};
}

std::vector<std::string> MediaSource::badges() const noexcept
{
    std::vector<std::string> out;
    if (subtitles_sdh)
        out.emplace_back("SDH");
    if (has_subtitles)
        out.emplace_back("CC");

    if (const std::string quality = quality_label(); !quality.empty())
        out.push_back(quality);

    /* Codec names the way a television would print them. */
    if (video_codec == "hevc" || video_codec == "h265")
        out.emplace_back("HEVC");
    else if (video_codec == "h264" || video_codec == "avc")
        out.emplace_back("H.264");
    else if (video_codec == "av1")
        out.emplace_back("AV1");
    else if (!video_codec.empty())
        out.push_back(video_codec);

    if (audio_codec == "eac3")
        out.emplace_back("DD+");
    else if (audio_codec == "ac3")
        out.emplace_back("DD");
    else if (audio_codec == "truehd")
        out.emplace_back("TrueHD");
    else if (audio_codec == "dca" || audio_codec == "dts")
        out.emplace_back("DTS");
    else if (audio_codec == "aac")
        out.emplace_back("AAC");
    else if (audio_codec == "flac")
        out.emplace_back("FLAC");

    if (audio_channels == 8)
        out.emplace_back("7.1");
    else if (audio_channels == 6)
        out.emplace_back("5.1");
    else if (audio_channels == 2)
        out.emplace_back("Stereo");

    return out;
}

const std::string &last_error() noexcept
{
    return g_error;
}

QuickConnectSession quick_connect_begin() noexcept
{
    QuickConnectSession session;
    const http::Response response = request_post("/QuickConnect/Initiate", "");
    if (!response.ok())
    {
        g_error = "could not start Quick Connect (status " + std::to_string(response.status) + ")";
        return session;
    }
    const json::ValuePtr root = json::parse(response.body);
    if (root == nullptr)
    {
        g_error = "Quick Connect reply did not parse";
        return session;
    }
    session.secret = std::string{root->str("Secret")};
    session.code = std::string{root->str("Code")};
    session.valid = !session.secret.empty() && !session.code.empty();
    if (!session.valid)
        g_error = "Quick Connect is not enabled on this server";
    return session;
}

bool quick_connect_poll(const QuickConnectSession &session) noexcept
{
    if (!session.valid)
        return false;
    const http::Response response =
        request_get("/QuickConnect/Connect?secret=" + http::url_encode(session.secret));
    if (!response.ok())
        return false;
    const json::ValuePtr state = json::parse(response.body);
    if (state == nullptr || !state->flag("Authenticated", false))
        return false;

    const http::Response authenticated =
        request_post("/Users/AuthenticateWithQuickConnect",
                     "{\"Secret\":\"" + json::escape(session.secret) + "\"}");
    if (!authenticated.ok())
    {
        g_error = "approval accepted but sign-in failed (status " +
                  std::to_string(authenticated.status) + ")";
        return false;
    }
    const json::ValuePtr result = json::parse(authenticated.body);
    if (result == nullptr)
    {
        g_error = "sign-in reply did not parse";
        return false;
    }
    config::Settings &settings = config::current();
    settings.token = std::string{result->str("AccessToken")};
    if (const json::Value *user = result->find("User"); user != nullptr)
        settings.user_id = std::string{user->str("Id")};
    if (!settings.signed_in())
    {
        g_error = "sign-in reply was missing the token or user";
        return false;
    }
    config::save();
    return true;
}

bool verify_session() noexcept
{
    const config::Settings &settings = config::current();
    if (!settings.signed_in())
        return false;
    const http::Response response = request_get("/Users/" + settings.user_id);
    if (response.status == 401)
    {
        config::Settings &mutable_settings = config::current();
        mutable_settings.token.clear();
        mutable_settings.user_id.clear();
        config::save();
        g_error = "stored sign-in was rejected";
        return false;
    }
    return response.ok();
}

bool authenticate(const std::string &username, const std::string &password) noexcept
{
    const std::string body = "{\"Username\":\"" + json::escape(username) + "\",\"Pw\":\"" +
                             json::escape(password) + "\"}";
    const http::Response response = request_post("/Users/AuthenticateByName", body);
    if (response.status == 401)
    {
        g_error = "That username or password was not accepted.";
        return false;
    }
    if (!response.ok())
    {
        g_error = "Sign-in failed (status " + std::to_string(response.status) + ").";
        return false;
    }
    const json::ValuePtr result = json::parse(response.body);
    if (result == nullptr)
    {
        g_error = "Sign-in reply did not parse.";
        return false;
    }
    config::Settings &settings = config::current();
    settings.token = std::string{result->str("AccessToken")};
    if (const json::Value *user = result->find("User"); user != nullptr)
        settings.user_id = std::string{user->str("Id")};
    if (!settings.signed_in())
    {
        g_error = "Sign-in reply was missing the token or user.";
        return false;
    }
    config::save();
    return true;
}

bool probe(const std::string &host, int port, std::string &server_name) noexcept
{
    const http::Response response = http::get(host, port, "/System/Info/Public", {});
    if (!response.ok())
    {
        g_error = "No Jellyfin server answered at that address.";
        return false;
    }
    const json::ValuePtr root = json::parse(response.body);
    if (root == nullptr || root->str("Id").empty())
    {
        g_error = "Something answered, but it is not a Jellyfin server.";
        return false;
    }
    server_name = std::string{root->str("ServerName")};
    return true;
}

bool mark_played(const std::string &item_id) noexcept
{
    /* Marking an item played is what removes it from Continue Watching. */
    const config::Settings &settings = config::current();
    const http::Response response =
        request_post("/Users/" + settings.user_id + "/PlayedItems/" + item_id, "");
    if (!response.ok())
    {
        g_error = "Could not update that item (status " + std::to_string(response.status) + ").";
        return false;
    }
    return true;
}

void sign_out() noexcept
{
    config::Settings &settings = config::current();
    settings.token.clear();
    settings.user_id.clear();
    config::save();
}

std::vector<Item> search(const std::string &term, int limit, const std::string &library_id,
                         const std::string &types) noexcept
{
    g_error.clear();
    if (term.empty())
        return {};
    const config::Settings &settings = config::current();
    /*
     * No sortBy on purpose. Jellyfin ranks a searchTerm query by its own match
     * score only while nothing else is asked for; naming a sort replaces that
     * ranking with an alphabetical one, which is how a search for "batman"
     * ends up leading with whatever happens to start with an A.
     */
    std::string path = "/Users/" + settings.user_id +
                       "/Items?searchTerm=" + http::url_encode(term) +
                       "&limit=" + std::to_string(limit) + "&recursive=true&includeItemTypes=" +
                       http::url_encode(types.empty() ? "Movie,Series,Episode" : types) +
                       "&fields=Overview&enableImages=true";
    if (!library_id.empty())
        path += "&parentId=" + http::url_encode(library_id);
    const http::Response response = request_get(path);
    if (!response.ok())
    {
        g_error = "Search failed (status " + std::to_string(response.status) + ").";
        return {};
    }
    return read_items(response.body);
}

bool details(const std::string &item_id, Item &out) noexcept
{
    const config::Settings &settings = config::current();
    const http::Response response =
        request_get("/Users/" + settings.user_id + "/Items/" + item_id +
                    "?fields=MediaSources,Overview,Genres,People,DateCreated,Studios,Taglines,"
                    "RecursiveItemCount,Chapters");
    if (!response.ok())
    {
        g_error = "Could not load that title (status " + std::to_string(response.status) + ").";
        return false;
    }
    const json::ValuePtr root = json::parse(response.body);
    if (root == nullptr)
        return false;
    out = read_item(*root);
    if (out.type == "Episode")
    {
        /* Media Segments are authoritative. Named chapters parsed above fill
           the holes when the server has no segment provider for this item. */
        const auto markers = intro::with_fallback(
            media_markers(out.id, root->num("RunTimeTicks", 0.0) / intro::kTicksPerSecond),
            {out.intro_start_seconds, out.intro_end_seconds, out.credits_start_seconds});
        out.intro_start_seconds = markers.intro_start;
        out.intro_end_seconds = markers.intro_end;
        out.credits_start_seconds = markers.outro_start;
    }
    return !out.id.empty();
}

std::vector<Item> seasons(const std::string &series_id) noexcept
{
    const config::Settings &settings = config::current();
    const http::Response response =
        request_get("/Shows/" + series_id + "/Seasons?userId=" + settings.user_id +
                    "&fields=Overview&enableImages=true");
    if (!response.ok())
        return {};
    return read_items(response.body);
}

std::vector<Item> episodes(const std::string &series_id, const std::string &season_id) noexcept
{
    const config::Settings &settings = config::current();
    std::string path = "/Shows/" + series_id + "/Episodes?userId=" + settings.user_id +
                       "&fields=Overview&enableImages=true";
    if (!season_id.empty())
        path += "&seasonId=" + http::url_encode(season_id);
    const http::Response response = request_get(path);
    if (!response.ok())
        return {};
    return read_items(response.body);
}

std::vector<Library> libraries() noexcept
{
    std::vector<Library> out;
    const config::Settings &settings = config::current();
    const http::Response response = request_get("/UserViews?userId=" + settings.user_id);
    if (!response.ok())
    {
        g_error = "could not list libraries (status " + std::to_string(response.status) + ")";
        return out;
    }
    const json::ValuePtr root = json::parse(response.body);
    if (root == nullptr)
        return out;
    const json::Value *list = root->find("Items");
    if (list == nullptr)
        return out;
    for (std::size_t i = 0; i < list->size(); ++i)
    {
        const json::Value *node = list->at(i);
        if (node == nullptr)
            continue;
        Library library;
        library.id = std::string{node->str("Id")};
        library.name = std::string{node->str("Name")};
        library.collection_type = std::string{node->str("CollectionType")};
        if (!library.id.empty())
            out.push_back(std::move(library));
    }
    return out;
}

std::vector<Item> latest(const std::string &library_id, int limit) noexcept
{
    const config::Settings &settings = config::current();
    /*
     * groupItems collapses episodes into the series they belong to, which is
     * what "latest in Dragonball" means: the show that gained an episode, not
     * the episode. Without it a library of one show reports the same show over
     * and over under a dozen episode titles. A library whose items are not
     * episodes is unaffected, so films still arrive as films.
     */
    std::string path = "/Users/" + settings.user_id +
                       "/Items/Latest?limit=" + std::to_string(limit) + "&groupItems=true" +
                       "&fields=Overview,PrimaryImageAspectRatio&enableImages=true";
    if (!library_id.empty())
        path += "&parentId=" + http::url_encode(library_id);
    const http::Response response = request_get(path);
    if (!response.ok())
    {
        g_error = "could not load latest (status " + std::to_string(response.status) + ")";
        return {};
    }
    return read_items(response.body);
}

std::vector<Item> resume(int limit) noexcept
{
    const config::Settings &settings = config::current();
    /* Pin the order: newest first, so the row cannot drift with server defaults. */
    const std::string path = "/Users/" + settings.user_id +
                             "/Items/Resume?limit=" + std::to_string(limit) +
                             "&mediaTypes=Video&fields=Overview&enableImages=true" +
                             "&sortBy=DatePlayed&sortOrder=Descending";
    const http::Response response = request_get(path);
    if (!response.ok())
        return {};
    return read_items(response.body);
}

std::vector<Item> next_up(int limit) noexcept
{
    const config::Settings &settings = config::current();
    const std::string path = "/Shows/NextUp?userId=" + settings.user_id +
                             "&limit=" + std::to_string(limit) +
                             "&fields=Overview&enableImages=true";
    const http::Response response = request_get(path);
    if (!response.ok())
        return {};
    return read_items(response.body);
}

std::vector<Item> next_up(int limit, const std::string &series_id) noexcept
{
    const config::Settings &settings = config::current();
    const std::string path =
        "/Shows/NextUp?userId=" + settings.user_id + "&seriesId=" + http::url_encode(series_id) +
        "&limit=" + std::to_string(limit) + "&fields=Overview&enableImages=true";
    const http::Response response = request_get(path);
    if (!response.ok())
        return {};
    return read_items(response.body);
}

std::vector<Item> items_in(const std::string &library_id, int start, int limit,
                           int *total_out) noexcept
{
    const config::Settings &settings = config::current();
    const std::string path =
        "/Users/" + settings.user_id + "/Items?parentId=" + http::url_encode(library_id) +
        "&startIndex=" + std::to_string(start) + "&limit=" + std::to_string(limit) +
        "&recursive=true&includeItemTypes=Movie,Series&sortBy=SortName&sortOrder=Ascending" +
        "&fields=Overview&enableImages=true";
    const http::Response response = request_get(path);
    if (!response.ok())
    {
        g_error = "could not load library (status " + std::to_string(response.status) + ")";
        return {};
    }
    if (total_out != nullptr)
    {
        *total_out = 0;
        /* The server counts the whole library here whatever the page asked
           for, which is the number worth showing beside a category's name. */
        if (const json::ValuePtr root = json::parse(response.body); root != nullptr)
            *total_out = static_cast<int>(root->num("TotalRecordCount", 0.0));
    }
    return read_items(response.body);
}

std::vector<Item> catalogue(const std::string &types, int start, int limit, int *total_out) noexcept
{
    const config::Settings &settings = config::current();
    /* Only the primary image's tag is wanted, so no other fields and no
       other image types: a page of a hundred stays under a hundred KiB. */
    const std::string path =
        "/Users/" + settings.user_id + "/Items?recursive=true&includeItemTypes=" + types +
        "&sortBy=SortName&sortOrder=Ascending&startIndex=" + std::to_string(start) +
        "&limit=" + std::to_string(limit) +
        "&enableImageTypes=Primary&imageTypeLimit=1&enableUserData=false&fields=";
    const http::Response response = request_get(path);
    if (total_out != nullptr)
        *total_out = 0;
    if (!response.ok())
        return {};
    if (total_out != nullptr)
    {
        if (const json::ValuePtr root = json::parse(response.body); root != nullptr)
            *total_out = static_cast<int>(root->num("TotalRecordCount", 0.0));
    }
    return read_items(response.body);
}

std::vector<unsigned char> image(const std::string &item_id, const std::string &tag,
                                 const char *kind, int max_height, int *status_out) noexcept
{
    const config::Settings &settings = config::current();
    std::string path = "/Items/" + item_id + "/Images/" + kind +
                       "?maxHeight=" + std::to_string(max_height) + "&quality=90";
    // stb is built with JPEG/PNG only. Preserve alpha for logos and avoid
    // inheriting an unsupported original format from the server's artwork.
    path += std::strcmp(kind, "Logo") == 0 ? "&format=Png" : "&format=Jpg";
    if (!tag.empty())
        path += "&tag=" + http::url_encode(tag);
    const http::Response response = http::get(settings.host, settings.port, path, auth_headers());
    if (status_out != nullptr)
        *status_out = response.status;
    if (!response.ok())
        return {};
    return {response.body.begin(), response.body.end()};
}

/* Advertise only codec, profile and channel limits supported by the active decode path. */
std::string device_profile(const std::string &codec_order, const std::string &audio_codec_order,
                           long long max_bitrate, int max_audio_channels,
                           const std::string &video_ranges = "SDR|HDR10",
                           const std::string &extra_audio = "",
                           const std::string &direct_audio = "aac,mp3,ac3") noexcept
{
    /* Preserve source timestamps during remuxing so subtitles, seeks and progress share title time.
     */
    std::string body = R"({"DeviceProfile":{
"MaxStreamingBitrate":@BITRATE@,
"MaxStaticBitrate":@BITRATE@,
"DirectPlayProfiles":[
 {"Container":"ts,mpegts","Type":"Video","VideoCodec":"h264,hevc","AudioCodec":"@DIRECTAUDIO@@EXTRAAUDIO@"}],
"TranscodingProfiles":[
 {"Container":"ts","Type":"Video","VideoCodec":"@CODECS@","AudioCodec":"@AUDIOCODECS@",
  "Protocol":"http","Context":"Streaming","MaxAudioChannels":"@CHANNELS@",
  "CopyTimestamps":true,"EnableSubtitlesInManifest":false}],
"CodecProfiles":[
 {"Type":"Video","Codec":"h264","Conditions":[
   {"Condition":"EqualsAny","Property":"VideoProfile","Value":"high|main|baseline|constrained baseline","IsRequired":false},
   {"Condition":"LessThanEqual","Property":"VideoLevel","Value":"52","IsRequired":false},
   {"Condition":"LessThanEqual","Property":"Width","Value":"3840","IsRequired":false}]},
 {"Type":"Video","Codec":"hevc","Conditions":[
   {"Condition":"EqualsAny","Property":"VideoProfile","Value":"main|main 10","IsRequired":false},
   {"Condition":"LessThanEqual","Property":"VideoLevel","Value":"153","IsRequired":false},
   {"Condition":"LessThanEqual","Property":"VideoBitDepth","Value":"10","IsRequired":false},
   {"Condition":"EqualsAny","Property":"VideoRangeType","Value":"@VIDEORANGES@","IsRequired":true},
   {"Condition":"LessThanEqual","Property":"Width","Value":"3840","IsRequired":false}]},
 {"Type":"VideoAudio","Codec":"aac","Conditions":[
   {"Condition":"LessThanEqual","Property":"AudioChannels","Value":"@CHANNELS@","IsRequired":false},
   {"Condition":"Equals","Property":"AudioSampleRate","Value":"48000","IsRequired":true}]},
 {"Type":"VideoAudio","Codec":"mp3","Conditions":[
   {"Condition":"LessThanEqual","Property":"AudioChannels","Value":"2","IsRequired":false},
   {"Condition":"Equals","Property":"AudioSampleRate","Value":"48000","IsRequired":false}]},
 {"Type":"VideoAudio","Codec":"ac3","Conditions":[
   {"Condition":"LessThanEqual","Property":"AudioChannels","Value":"6","IsRequired":true},
   {"Condition":"Equals","Property":"AudioSampleRate","Value":"48000","IsRequired":true}]}],
"SubtitleProfiles":[
 {"Format":"srt","Method":"External"},
 {"Format":"subrip","Method":"External"},
 {"Format":"pgssub","Method":"Encode"},
 {"Format":"dvdsub","Method":"Encode"},
 {"Format":"dvbsub","Method":"Encode"}]}})";
    const auto replace = [&body](const std::string &token, const std::string &value)
    {
        for (std::size_t at = body.find(token); at != std::string::npos; at = body.find(token))
            body.replace(at, token.size(), value);
    };
    replace("@DIRECTAUDIO@", direct_audio);
    replace("@EXTRAAUDIO@", extra_audio.empty() ? "" : "," + extra_audio);
    replace("@CODECS@", codec_order);
    replace("@AUDIOCODECS@", audio_codec_order);
    replace("@VIDEORANGES@", video_ranges);
    replace("@BITRATE@", std::to_string(max_bitrate > 0 ? max_bitrate : 120000000LL));
    replace("@CHANNELS@", std::to_string(max_audio_channels > 0 ? max_audio_channels : 8));
    return body;
}

/* Reads one negotiation result. Returns false when the server said nothing. */
bool read_plan(const std::string &body, const PlaybackRequest &request, PlaybackPlan &plan) noexcept
{
    const json::ValuePtr root = json::parse(body);
    if (root == nullptr)
        return false;
    const json::Value *sources = root->find("MediaSources");
    if (sources == nullptr || sources->size() == 0)
        return false;

    /* A title with several versions returns all of them; use the one asked for. */
    const json::Value *source = request.media_source_id.empty() ? sources->at(0) : nullptr;
    for (std::size_t i = 0; i < sources->size() && !request.media_source_id.empty(); ++i)
    {
        const json::Value *candidate = sources->at(i);
        if (candidate != nullptr && candidate->str("Id") == request.media_source_id)
            source = candidate;
    }
    if (source == nullptr)
        return false;

    plan.play_session = std::string{root->str("PlaySessionId")};
    plan.media_source_id = std::string{source->str("Id")};

    const int selected_audio = request.audio_index >= 0
                                   ? request.audio_index
                                   : static_cast<int>(source->num("DefaultAudioStreamIndex", -1));
    if (const json::Value *streams = source->find("MediaStreams"); streams != nullptr)
    {
        for (std::size_t i = 0; i < streams->size(); ++i)
        {
            const json::Value *stream = streams->at(i);
            if (stream == nullptr)
                continue;
            const std::string kind{stream->str("Type")};
            const int index = static_cast<int>(stream->num("Index", -1.0));
            if (kind == "Video" && plan.video_codec.empty())
            {
                plan.video_codec = std::string{stream->str("Codec")};
                plan.bit_depth = static_cast<int>(stream->num("BitDepth", 8));
                plan.width = static_cast<int>(stream->num("Width", 0.0));
                plan.height = static_cast<int>(stream->num("Height", 0.0));
                plan.frame_rate = stream->num("RealFrameRate", 0.0);
                if (plan.frame_rate <= 0.0)
                    plan.frame_rate = stream->num("AverageFrameRate", 0.0);
                const std::string range{stream->str("VideoRangeType")};
                plan.source_video_range = range;
                plan.hdr = range == "HDR10" || range == "HDR10Plus" || range == "PQ";
                plan.dv_hdr10_base =
                    request.allow_dv_hdr10_base && plan.video_codec == "hevc" &&
                    video::hdr10_base_layer(
                        range, static_cast<int>(stream->num("DvProfile", -1)),
                        static_cast<int>(stream->num("DvBlSignalCompatibilityId", -1)),
                        stream->str("ColorTransfer"), stream->str("ColorPrimaries"),
                        plan.bit_depth);
                plan.hdr |= plan.dv_hdr10_base;
            }
            else if (kind == "Audio" &&
                     (selected_audio >= 0 ? index == selected_audio : plan.audio_codec.empty()))
            {
                plan.audio_codec = std::string{stream->str("Codec")};
                plan.audio_profile = std::string{stream->str("Profile")};
                plan.audio_channels = static_cast<int>(stream->num("Channels", 2));
                plan.audio_sample_rate = static_cast<int>(stream->num("SampleRate", 0));
            }
            else if (kind == "Subtitle" && index == request.subtitle_index)
            {
                const std::string method{stream->str("DeliveryMethod")};
                if (method == "External")
                    plan.subtitle_url = std::string{stream->str("DeliveryUrl")};
                else if (method == "Encode")
                    plan.subtitle_burned = true;
            }
        }
    }

    const std::string transcode{source->str("TranscodingUrl")};
    if (!transcode.empty())
    {
        /* The stream endpoint defaults to burning subtitles in; send a track index only for
         * server-rendered tracks. */
        plan.path =
            with_subtitle_index(transcode, plan.subtitle_burned ? request.subtitle_index : -1);
        if (!plan.audio_codec.empty() && plan.audio_sample_rate != 48000)
        {
            // PlaybackInfo's sample-rate condition alone can still produce an
            // AAC-copy URL. The sink cannot consume 44.1 kHz PCM; force only
            // audio through resampling, leaving video copy available.
            plan.path = with_query_value(plan.path, "AllowAudioStreamCopy", "false");
            plan.path = with_query_value(plan.path, "AudioSampleRate", "48000");
        }
        plan.direct = false;
        std::string lowered = transcode;
        for (char &c : lowered)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        plan.absolute_timestamps = lowered.find("copytimestamps=true") != std::string::npos;
        plan.valid = true;
        return true;
    }
    if (source->flag("SupportsDirectStream") || source->flag("SupportsDirectPlay"))
    {
        const std::string container{source->str("Container")};
        if ((container == "ts" || container == "mpegts") &&
            (plan.audio_codec.empty() || plan.audio_sample_rate == 48000))
        {
            plan.path = "/Videos/" + request.item_id +
                        "/stream.ts?static=true&mediaSourceId=" + plan.media_source_id;
            plan.direct = true;
            plan.valid = true;
            return true;
        }
    }
    return false;
}

std::string playback_delivery(PlaybackPlan &plan) noexcept
{
    if (plan.direct)
    {
        plan.video_delivery_known = true;
        return "Video " + plan.video_codec + " copy | Audio " + plan.audio_codec +
               " copy | Direct play: source already fits client";
    }
    const auto &settings = config::current();
    const http::Response response =
        request_get("/Sessions?deviceId=" + http::url_encode(settings.device_id));
    const auto root = response.ok() ? json::parse(response.body) : nullptr;
    if (root != nullptr)
    {
        for (std::size_t i = 0; i < root->size(); ++i)
        {
            const auto *session = root->at(i);
            if (session == nullptr || session->str("DeviceId") != settings.device_id)
                continue;
            const auto *info = session->find("TranscodingInfo");
            if (info == nullptr || info->str("VideoCodec").empty())
                continue;
            // Sessions can briefly retain the previous stream after a restart.
            const auto *state = session->find("PlayState");
            if (state && !state->str("MediaSourceId").empty() &&
                state->str("MediaSourceId") != plan.media_source_id)
                continue;
            const std::string expected_audio = query_value(plan.path, "AudioCodec");
            if (!expected_audio.empty() &&
                info->str("AudioCodec") != expected_audio.substr(0, expected_audio.find(',')))
                continue;
            if (query_value(plan.path, "AllowAudioStreamCopy") == "false" &&
                info->flag("IsAudioDirect"))
                continue;
            const std::string expected_reasons = query_value(plan.path, "TranscodeReasons");
            bool reasons_match = true;
            for (std::size_t from = 0; from < expected_reasons.size();)
            {
                const auto comma = expected_reasons.find(',', from);
                const auto reason = expected_reasons.substr(
                    from, comma == std::string::npos ? std::string::npos : comma - from);
                bool found = false;
                if (const auto *actual = info->find("TranscodeReasons"))
                    for (std::size_t j = 0; j < actual->size(); ++j)
                        if (actual->at(j) && actual->at(j)->string_or("") == reason)
                            found = true;
                reasons_match &= found;
                if (comma == std::string::npos)
                    break;
                from = comma + 1;
            }
            if (!reasons_match)
                continue;
            plan.video_delivery_known = true;
            if (!info->flag("IsVideoDirect"))
            {
                // Jellyfin video transcodes are SDR; source PQ is no longer applicable.
                plan.hdr = false;
                plan.dv_hdr10_base = false;
                plan.bit_depth = static_cast<int>(info->num("BitDepth", 8));
            }
            std::string line = "Video " + std::string{info->str("VideoCodec")} +
                               (info->flag("IsVideoDirect") ? " copy" : " transcode");
            if (plan.dv_hdr10_base)
                line += " (DV HDR10 base)";
            line += " | Audio " + plan.audio_codec;
            if (info->flag("IsAudioDirect"))
                line += " copy";
            else
                line += " -> " + std::string{info->str("AudioCodec")} +
                        " transcode (native client decoder)";
            std::string reasons;
            if (const auto *values = info->find("TranscodeReasons"); values != nullptr)
                for (std::size_t j = 0; j < values->size(); ++j)
                {
                    const auto *reason = values->at(j);
                    if (reason == nullptr)
                        continue;
                    if (!reasons.empty())
                        reasons += ", ";
                    reasons += reason->string_or("");
                }
            if (!reasons.empty())
                line += " | Server: " + reasons;
            return line;
        }
    }
    return "Video/audio delivery unconfirmed: server live session status unavailable";
}

PlaybackPlan playback_plan(const PlaybackRequest &request) noexcept
{
    PlaybackPlan plan;
    const config::Settings &settings = config::current();
    const auto ticks = static_cast<long long>(request.start_seconds * 10000000.0);
    const long long bitrate = request.max_bitrate > 0 ? request.max_bitrate : 120000000LL;
    std::string path = "/Items/" + request.item_id + "/PlaybackInfo?userId=" + settings.user_id +
                       "&startTimeTicks=" + std::to_string(ticks) +
                       "&autoOpenLiveStream=true&maxStreamingBitrate=" + std::to_string(bitrate) +
                       "&SubtitleStreamIndex=" + std::to_string(request.subtitle_index);
    if (request.audio_index >= 0)
        path += "&AudioStreamIndex=" + std::to_string(request.audio_index);
    if (!request.media_source_id.empty())
        path += "&MediaSourceId=" + http::url_encode(request.media_source_id);

    /*
     * Negotiate twice. The first call is only to learn what the source actually
     * is; the second asks again with that codec listed first, which is what
     * makes the server copy the stream rather than re-encode it. Two round
     * trips on a local network cost a few milliseconds and save the server a
     * whole 4K encode.
     */
    const int channels = request.max_audio_channels;

    const http::Response first = request_post(
        path, device_profile(request.video_codec.empty() ? "hevc,h264" : request.video_codec, "aac",
                             bitrate, channels));
    if (first.ok() && read_plan(first.body, request, plan))
    {
        const bool source_is_h264 = plan.video_codec.find("h264") != std::string::npos ||
                                    plan.video_codec.find("avc") != std::string::npos;
        const bool source_is_mp3 = plan.audio_codec == "mp3";
        const bool source_is_ac3 = plan.audio_codec == "ac3";
        const bool source_is_truehd = plan.audio_codec == "truehd";
        // Restrict the first movie trial to the tested 48 kHz codecs/layout
        // sizes. A user channel limit still requires server conversion.
        /* Convert TrueHD to E-AC-3 for bitstream output. TrueHD MAT passthrough is unsupported;
         * preserve PCM/server fallback. */
        const bool truehd_to_bitstream = source_is_truehd && request.allow_bitstream;
        const bool software_audio = request.allow_software_audio && !truehd_to_bitstream &&
                                    plan.audio_sample_rate == 48000 && plan.audio_channels >= 1 &&
                                    plan.audio_channels <= 8 &&
                                    (channels <= 0 || plan.audio_channels <= channels) &&
                                    (source_is_truehd || plan.audio_codec == "eac3" ||
                                     (plan.audio_codec == "dts" && plan.audio_profile == "DTS"));
        /* Bitstream: the TV decodes E-AC-3 itself, and DTS from its core,
           so any DTS profile can be copied, DTS-HD MA included. */
        const bool bitstream_audio = request.allow_bitstream && plan.audio_sample_rate == 48000 &&
                                     plan.audio_channels >= 1 && plan.audio_channels <= 8 &&
                                     (channels <= 0 || plan.audio_channels <= channels) &&
                                     (plan.audio_codec == "eac3" || plan.audio_codec == "dts");
        const bool copy_audio = software_audio || bitstream_audio;
        /*
         * A format chosen in the Audio panel: the server converts to exactly
         * that, and nothing else may be copied. AC-3 and E-AC-3 encoders stop
         * at 5.1. The same format as the track is simply the original.
         */
        /* A TrueHD source with nothing chosen by hand asks for E-AC-3. */
        if (request.audio_codec.empty() && truehd_to_bitstream)
        {
            PlaybackRequest resolved = request;
            resolved.media_source_id = plan.media_source_id;
            const std::string second_path =
                request.media_source_id.empty()
                    ? path + "&MediaSourceId=" + http::url_encode(resolved.media_source_id)
                    : path;
            PlaybackPlan converted;
            const http::Response again = request_post(
                second_path,
                device_profile(!request.video_codec.empty() ? request.video_codec
                               : source_is_h264             ? "h264,hevc"
                                                            : "hevc,h264",
                               "eac3", bitrate, channels > 0 ? std::min(channels, 6) : 6,
                               plan.dv_hdr10_base ? "SDR|HDR10|" + plan.source_video_range
                                                  : "SDR|HDR10",
                               "", "eac3"));
            if (again.ok() && read_plan(again.body, resolved, converted))
                plan = converted;
            return plan;
        }
        if (!request.audio_codec.empty() && request.audio_codec != plan.audio_codec)
        {
            PlaybackRequest resolved = request;
            resolved.media_source_id = plan.media_source_id;
            const std::string second_path =
                request.media_source_id.empty()
                    ? path + "&MediaSourceId=" + http::url_encode(resolved.media_source_id)
                    : path;
            const bool dolby = request.audio_codec == "ac3" || request.audio_codec == "eac3";
            const int limit = dolby ? (channels > 0 ? std::min(channels, 6) : 6) : channels;
            PlaybackPlan converted;
            const http::Response again = request_post(
                second_path,
                device_profile(!request.video_codec.empty() ? request.video_codec
                               : source_is_h264             ? "h264,hevc"
                                                            : "hevc,h264",
                               request.audio_codec, bitrate, limit,
                               plan.dv_hdr10_base ? "SDR|HDR10|" + plan.source_video_range
                                                  : "SDR|HDR10",
                               "", request.audio_codec));
            if (again.ok() && read_plan(again.body, resolved, converted))
                plan = converted;
            return plan;
        }
        if (source_is_h264 || source_is_mp3 || source_is_ac3 || source_is_truehd ||
            plan.dv_hdr10_base || copy_audio)
        {
            PlaybackPlan second;
            PlaybackRequest resolved = request;
            resolved.media_source_id = plan.media_source_id;
            const std::string second_path =
                request.media_source_id.empty()
                    ? path + "&MediaSourceId=" + http::url_encode(resolved.media_source_id)
                    : path;
            const http::Response again = request_post(
                second_path,
                device_profile(
                    !request.video_codec.empty() ? request.video_codec
                    : source_is_h264             ? "h264,hevc"
                                                 : "hevc,h264",
                    copy_audio         ? plan.audio_codec + ",ac3,aac"
                    : source_is_truehd ? "ac3"
                    : source_is_ac3    ? "ac3,aac"
                    : source_is_mp3    ? "mp3,aac"
                                       : "aac",
                    bitrate,
                    source_is_truehd && !copy_audio ? (channels > 0 ? std::min(channels, 6) : 6)
                                                    : channels,
                    plan.dv_hdr10_base ? "SDR|HDR10|" + plan.source_video_range : "SDR|HDR10",
                    copy_audio ? plan.audio_codec : ""));
            if (again.ok() && read_plan(again.body, resolved, second))
                plan = second;
        }
        return plan;
    }

    /* A guessed fallback silently ignored quality, codec and subtitle choices.
       Keep those choices authoritative and let the caller report the failure. */
    return PlaybackPlan{};
}

std::string fetch_text(const std::string &path) noexcept
{
    const http::Response response = request_get(path);
    /* Say what came back: an empty result is otherwise indistinguishable
       between a refusal, a redirect and a genuinely empty document. */
    trace::mark("fetch_text status " + std::to_string(response.status) + " bytes " +
                std::to_string(response.body.size()) + " path " + std::to_string(path.size()) +
                " " + path.substr(0, 70));
    return response.ok() ? response.body : std::string{};
}

bool post_json(const std::string &path, const std::string &body) noexcept
{
    return request_post(path, body).ok();
}

/* ------------------------------------------------ account  */

namespace
{
User read_user(const json::Value &node) noexcept
{
    User user;
    user.id = std::string{node.str("Id")};
    user.name = std::string{node.str("Name")};
    user.primary_image_tag = std::string{node.str("PrimaryImageTag")};
    user.last_activity = std::string{node.str("LastActivityDate")};
    user.last_login = std::string{node.str("LastLoginDate")};
    user.has_password = node.flag("HasPassword");
    if (const json::Value *policy = node.find("Policy"); policy != nullptr)
    {
        user.is_admin = policy->flag("IsAdministrator");
        user.is_disabled = policy->flag("IsDisabled");
        user.remote_access = policy->flag("EnableRemoteAccess", true);
        user.can_download = policy->flag("EnableContentDownloading");
        user.live_tv = policy->flag("EnableLiveTvAccess");
        user.can_play = policy->flag("EnableMediaPlayback", true);
    }
    return user;
}

std::string status_error(const http::Response &response, const char *what) noexcept
{
    if (response.status == 0)
        return std::string{"Could not reach the server to "} + what + ".";
    if (response.status == 401 || response.status == 403)
        return std::string{"The server refused to "} + what + ".";
    return std::string{"Could not "} + what + " (status " + std::to_string(response.status) + ").";
}

std::string base64(const std::vector<unsigned char> &bytes) noexcept
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
                               (static_cast<unsigned>(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(kAlphabet[(value >> 18) & 63]);
        out.push_back(kAlphabet[(value >> 12) & 63]);
        out.push_back(kAlphabet[(value >> 6) & 63]);
        out.push_back(kAlphabet[value & 63]);
    }
    if (i < bytes.size())
    {
        const bool two = i + 1 < bytes.size();
        const unsigned value = (static_cast<unsigned>(bytes[i]) << 16) |
                               (two ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0u);
        out.push_back(kAlphabet[(value >> 18) & 63]);
        out.push_back(kAlphabet[(value >> 12) & 63]);
        out.push_back(two ? kAlphabet[(value >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}
} // namespace

std::vector<std::string> authorization() noexcept
{
    return auth_headers();
}

http::Response api(std::string_view method, const std::string &path, const std::string &body,
                   std::string_view content_type) noexcept
{
    const config::Settings &settings = config::current();
    return http::send(method, settings.host, settings.port, path, auth_headers(), body,
                      content_type);
}

bool current_user(User &out, std::string &error) noexcept
{
    const config::Settings &settings = config::current();
    const http::Response response = request_get("/Users/" + settings.user_id);
    const json::ValuePtr root = response.ok() ? json::parse(response.body) : nullptr;
    if (!root)
    {
        error = status_error(response, "load your profile");
        return false;
    }
    out = read_user(*root);
    return true;
}

std::vector<User> users(std::string &error) noexcept
{
    const http::Response response = request_get("/Users");
    const json::ValuePtr root = response.ok() ? json::parse(response.body) : nullptr;
    if (!root)
    {
        error = status_error(response, "list users");
        return {};
    }
    std::vector<User> out;
    for (const json::ValuePtr &node : root->elements)
        if (node)
            out.push_back(read_user(*node));
    return out;
}

std::vector<unsigned char> user_image(const std::string &user_id, const std::string &tag) noexcept
{
    std::string path = "/UserImage?userId=" + http::url_encode(user_id) + "&format=Jpg";
    if (!tag.empty())
        path += "&tag=" + http::url_encode(tag);
    const http::Response response = request_get(path);
    if (!response.ok())
        return {};
    return {response.body.begin(), response.body.end()};
}

bool change_password(const std::string &current, const std::string &next,
                     std::string &error) noexcept
{
    const config::Settings &settings = config::current();
    const std::string body = "{\"CurrentPw\":\"" + json::escape(current) + "\",\"NewPw\":\"" +
                             json::escape(next) + "\"}";
    const http::Response response =
        api("POST", "/Users/Password?userId=" + http::url_encode(settings.user_id), body);
    if (response.ok())
        return true;
    error = response.status == 403 || response.status == 401
                ? "The current password was not right."
                : status_error(response, "change the password");
    return false;
}

bool upload_user_image(const std::vector<unsigned char> &jpeg, std::string &error) noexcept
{
    const config::Settings &settings = config::current();
    /* Jellyfin reads the picture as base64 text under the image's own type. */
    const http::Response response =
        api("POST", "/UserImage?userId=" + http::url_encode(settings.user_id), base64(jpeg),
            "image/jpeg");
    if (response.ok())
        return true;
    error = status_error(response, "save the picture");
    return false;
}

bool delete_user_image(std::string &error) noexcept
{
    const config::Settings &settings = config::current();
    const http::Response response =
        api("DELETE", "/UserImage?userId=" + http::url_encode(settings.user_id));
    if (response.ok())
        return true;
    error = status_error(response, "remove the picture");
    return false;
}

} // namespace slopfin::jellyfin
