/*
 * SlopFin - "On this PS5": how a file will reach the console, and why.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read from the same negotiation Play runs. The server lists its reasons in
 * the stream URL (TranscodeReasons), and it groups them itself in
 * MediaBrowser.Model/Dlna/StreamBuilder.cs (Jellyfin 10.11.10):
 *
 *   - any VideoReasons, or ContainerBitrateExceedsLimit: the video is encoded
 *     again (line 812 applies the transcoding conditions for exactly these);
 *   - any AudioReasons: the audio is converted;
 *   - ContainerNotSupported alone is in DirectStreamReasons: the streams are
 *     copied into a new container, which loses nothing.
 *
 * So nothing here is guessed from codecs; it restates the server's decision.
 * Header-only so the wording is tested against deliveries seen on the TV.
 */

#ifndef SLOPFIN_DELIVERY_SUMMARY_HPP
#define SLOPFIN_DELIVERY_SUMMARY_HPP

#include "details_sheet.hpp"
#include "jellyfin.hpp"
#include "playback_url.hpp"

#include <string>
#include <vector>

namespace slopfin::delivery
{

inline std::vector<std::string> reasons(const jellyfin::PlaybackPlan &plan)
{
    std::vector<std::string> out;
    const std::string list = jellyfin::query_value(plan.path, "TranscodeReasons");
    for (std::size_t at = 0; at < list.size();)
    {
        const std::size_t comma = list.find(',', at);
        std::string one =
            list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!one.empty())
            out.push_back(std::move(one));
        if (comma == std::string::npos)
            break;
        at = comma + 1;
    }
    return out;
}

inline bool has(const std::vector<std::string> &list, const char *name)
{
    for (const std::string &one : list)
        if (one == name)
            return true;
    return false;
}

/* StreamBuilder's VideoReasons plus ContainerBitrateExceedsLimit. */
inline bool video_encoded(const std::vector<std::string> &list)
{
    for (const char *name :
         {"VideoCodecNotSupported", "VideoResolutionNotSupported", "AnamorphicVideoNotSupported",
          "InterlacedVideoNotSupported", "VideoBitDepthNotSupported", "VideoBitrateNotSupported",
          "VideoFramerateNotSupported", "VideoLevelNotSupported", "RefFramesNotSupported",
          "VideoRangeTypeNotSupported", "VideoProfileNotSupported", "ContainerBitrateExceedsLimit"})
        if (has(list, name))
            return true;
    return false;
}

/* StreamBuilder's AudioReasons. */
inline bool audio_converted(const std::vector<std::string> &list)
{
    for (const char *name :
         {"AudioCodecNotSupported", "AudioBitrateNotSupported", "AudioChannelsNotSupported",
          "AudioProfileNotSupported", "AudioSampleRateNotSupported", "SecondaryAudioNotSupported",
          "AudioBitDepthNotSupported", "AudioIsExternal"})
        if (has(list, name))
            return true;
    return false;
}

/* One reason in words, for the "Why" lines. Unknown names are shown as sent. */
inline std::string explain(const std::string &reason)
{
    struct Entry
    {
        const char *name;
        const char *words;
    };
    static constexpr Entry kWords[] = {
        {"ContainerNotSupported", "The file is repackaged for streaming; nothing is lost"},
        {"VideoCodecNotSupported", "This PS5 cannot decode the video codec"},
        {"AudioCodecNotSupported", "This PS5 cannot play the audio format"},
        {"SubtitleCodecNotSupported", "Picture subtitles have to be drawn into the video"},
        {"AudioIsExternal", "The audio is in a separate file"},
        {"SecondaryAudioNotSupported", "The chosen audio track is not the first one"},
        {"StreamCountExceedsLimit", "The file has more streams than can be sent"},
        {"VideoProfileNotSupported", "The video profile is not supported"},
        {"VideoRangeTypeNotSupported", "The HDR type is not supported"},
        {"VideoCodecTagNotSupported", "The video is relabelled for this PS5"},
        {"VideoLevelNotSupported", "The video level is too high"},
        {"VideoResolutionNotSupported", "The resolution is too high"},
        {"VideoBitDepthNotSupported", "The colour bit depth is not supported"},
        {"VideoFramerateNotSupported", "The frame rate is not supported"},
        {"RefFramesNotSupported", "The video uses too many reference frames"},
        {"AnamorphicVideoNotSupported", "Anamorphic video is not supported"},
        {"InterlacedVideoNotSupported", "Interlaced video is not supported"},
        {"AudioChannelsNotSupported", "More audio channels than the channel limit"},
        {"AudioProfileNotSupported", "The audio profile is not supported"},
        {"AudioSampleRateNotSupported", "The audio sample rate needs converting"},
        {"AudioBitDepthNotSupported", "The audio bit depth is not supported"},
        {"ContainerBitrateExceedsLimit", "The file's bitrate is above the quality setting"},
        {"VideoBitrateNotSupported", "The video bitrate is above the quality setting"},
        {"AudioBitrateNotSupported", "The audio bitrate is above the limit"},
        {"UnknownVideoStreamInfo", "The server could not read the video details"},
        {"UnknownAudioStreamInfo", "The server could not read the audio details"},
        {"DirectPlayError", "Direct play failed"},
    };
    for (const Entry &entry : kWords)
        if (reason == entry.name)
            return entry.words;
    return reason;
}

/* Separate server conversion from the device that decodes the delivered stream.
   The sheet describes negotiation; live playback reports the actual output path. */
inline std::vector<details::Line> describe(const jellyfin::PlaybackPlan &plan, int subtitle_index,
                                           bool passthrough = false)
{
    std::vector<details::Line> out;
    const auto pair = [&out](std::string key, std::string value)
    { out.push_back({details::Line::Kind::pair, std::move(key), std::move(value)}); };
    if (!plan.valid)
    {
        pair("Server", "Could not ask the server how this would play");
        return out;
    }

    const std::vector<std::string> why = reasons(plan);
    const bool encode = video_encoded(why) || plan.subtitle_burned;
    const bool convert = audio_converted(why);
    const std::string audio_to = [&plan]
    {
        std::string list = jellyfin::query_value(plan.path, "AudioCodec");
        return list.substr(0, list.find(','));
    }();
    std::string audio_from = details::codec_name(plan.audio_codec);
    if (!plan.audio_profile.empty() && plan.audio_codec == "dts")
        audio_from = plan.audio_profile;

    if (plan.direct)
        pair("Plays as", "Direct play: the file exactly as it is");
    else if (encode)
        pair("Plays as", "Converted by the server: the video is encoded again");
    else if (convert)
        pair("Plays as", "Video copied, audio converted by the server");
    else
        pair("Plays as", "Copied into a streaming container: nothing is re-encoded");

    std::string video = details::codec_name(plan.video_codec);
    if (encode)
        video = "Encoded again by the server";
    else
    {
        video = "Copied (" + video + ")";
        if (plan.dv_hdr10_base)
            video += "  \xc2\xb7  Dolby Vision played as its HDR10 base";
        else if (plan.hdr)
            video += "  \xc2\xb7  HDR10";
    }
    pair("Video", video);

    std::string audio;
    if (convert)
        audio = audio_from + " converted by Jellyfin to " + details::codec_name(audio_to);
    else
    {
        audio = "Copied (" + audio_from + ")";
    }
    pair("Audio", audio);
    pair("Video decode", "PS5 hardware decoder (H.264 / HEVC)");
    const std::string delivered = convert ? audio_to : plan.audio_codec;
    if (passthrough && (delivered == "ac3" || delivered == "eac3" || delivered == "dts"))
        pair("Audio decode", "TV / receiver via HDMI passthrough (requested)");
    else if (delivered == "truehd" || delivered == "eac3" || delivered == "dts")
        pair("Audio decode", "PS5 CPU software decoder (FFmpeg) to PCM");
    else if (delivered == "aac" || delivered == "mp3" || delivered == "ac3")
        pair("Audio decode", "PS5 platform audio decoder to PCM");
    else
        pair("Audio decode", "Delivery format not confirmed");

    if (subtitle_index < 0)
        pair("Subtitles", "Off");
    else if (plan.subtitle_burned)
        pair("Subtitles", "Drawn into the picture by the server");
    else if (!plan.subtitle_url.empty())
        pair("Subtitles", "Text, drawn by SlopFin");

    for (std::size_t i = 0; i < why.size(); ++i)
        pair(i == 0 ? "Why" : "", explain(why[i]));
    return out;
}

} // namespace slopfin::delivery

#endif
