/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
// Exercise negotiation against recorded-shape server responses, without a PS5.
#include "../src/jellyfin.cpp"
#include <cassert>
#include <cstdio>

namespace
{
std::string response_body;
std::string session_body;
std::string last_get_path;
std::vector<std::string> requests, bodies;
} // namespace
namespace slopfin::config
{
Settings &current() noexcept
{
    static Settings settings;
    settings.device_id = "test";
    return settings;
}
} // namespace slopfin::config
namespace slopfin::http
{
Response post(std::string_view, int, std::string_view path, const std::vector<std::string> &,
              std::string_view body, std::string_view) noexcept
{
    requests.emplace_back(path);
    bodies.emplace_back(body);
    return {response_body.empty() ? 500 : 200, response_body};
}
Response get(std::string_view, int, std::string_view path,
             const std::vector<std::string> &) noexcept
{
    last_get_path = path;
    return {200, session_body};
}
std::string url_encode(std::string_view value) noexcept
{
    return std::string{value};
}
} // namespace slopfin::http
int main()
{
    using namespace slopfin::jellyfin;
    // Scope crops are labelled by their tier, not their height.
    const auto label = [](int w, int h)
    {
        MediaSource m;
        m.width = w;
        m.height = h;
        return m.quality_label();
    };
    assert(label(3840, 1600) == "4K" && label(3840, 2160) == "4K" && label(1920, 800) == "1080p");
    assert(label(2560, 1440) == "1440p" && label(1280, 536) == "720p" && label(720, 480) == "480p");
    (void)image("poster", "tag", "Primary", 480, nullptr);
    assert(last_get_path.find("format=Jpg") != std::string::npos);
    (void)image("logo", "tag", "Logo", 110, nullptr);
    assert(last_get_path.find("format=Png") != std::string::npos);

    response_body = R"({"PlaySessionId":"new","MediaSources":[{"Id":"source",
      "TranscodingUrl":"/stream.ts?AudioCodec=ac3&VideoCodec=hevc",
      "MediaStreams":[{"Type":"Video","Codec":"hevc","BitDepth":10,"VideoRangeType":"HDR10"},
      {"Type":"Audio","Index":1,"Codec":"truehd","Channels":8}]}]})";
    PlaybackRequest request;
    request.item_id = "item";
    request.audio_index = 1;
    request.max_bitrate = 4000000;
    request.video_codec = "h264";
    auto plan = playback_plan(request);
    assert(plan.valid && plan.hdr && plan.bit_depth == 10);
    assert(requests.size() == 2);
    for (const auto &path : requests)
        assert(path.find("maxStreamingBitrate=4000000") != std::string::npos);
    auto profile = slopfin::json::parse(bodies.back());
    const auto *transcode = profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0);
    assert(transcode->str("VideoCodec") == "h264");
    assert(transcode->str("AudioCodec") == "ac3");
    assert(transcode->str("MaxAudioChannels") == "6");
    assert(plan.path.find("SubtitleStreamIndex=-1") != std::string::npos);
    // CPU trial is selective, source-pinned and honors an explicit channel cap.
    const auto baseline_response = response_body;
    response_body = R"({"PlaySessionId":"cpu","MediaSources":[{"Id":"cpu-source",
      "TranscodingUrl":"/stream.ts?AudioCodec=truehd&VideoCodec=hevc",
      "MediaStreams":[{"Type":"Video","Codec":"hevc"},
      {"Type":"Audio","Index":1,"Codec":"truehd","Channels":8,"SampleRate":48000}]}]})";
    PlaybackRequest cpu;
    cpu.item_id = "cpu";
    cpu.audio_index = 1;
    cpu.allow_software_audio = true;
    assert(playback_plan(cpu).valid);
    auto cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "truehd,ac3,aac");
    assert(
        cpu_profile->find("DeviceProfile")->find("DirectPlayProfiles")->at(0)->str("AudioCodec") ==
        "aac,mp3,ac3,truehd");
    cpu.max_audio_channels = 2;
    assert(playback_plan(cpu).valid);
    cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "ac3");
    assert(cpu_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("MaxAudioChannels") == "2");
    cpu.max_audio_channels = 0;
    response_body =
        R"({"MediaSources":[{"Id":"dts-source","TranscodingUrl":"/stream.ts?AudioCodec=dts",
      "MediaStreams":[{"Type":"Video","Codec":"h264"},{"Type":"Audio","Index":1,
      "Codec":"dts","Profile":"DTS","SampleRate":48000,"Channels":6}]}]})";
    assert(playback_plan(cpu).valid);
    cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "dts,ac3,aac");
    response_body.replace(response_body.find("\"Profile\":\"DTS\""), 15,
                          "\"Profile\":\"DTS-HD MA\"");
    assert(playback_plan(cpu).valid);
    cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "aac");
    // Bitstream: the TV takes the DTS core, so DTS-HD MA is copied as well.
    PlaybackRequest hdmi;
    hdmi.item_id = "hdmi";
    hdmi.audio_index = 1;
    hdmi.allow_bitstream = true;
    assert(playback_plan(hdmi).valid);
    cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "dts,ac3,aac");
    hdmi.max_audio_channels = 2;
    assert(playback_plan(hdmi).valid);
    cpu_profile = slopfin::json::parse(bodies.back());
    assert(
        cpu_profile->find("DeviceProfile")->find("TranscodingProfiles")->at(0)->str("AudioCodec") ==
        "aac");
    // TrueHD with nothing chosen: the server converts it to E-AC-3, which the
    // TV decodes. The console's own TrueHD decoder is not asked for.
    response_body = R"({"PlaySessionId":"thd","MediaSources":[{"Id":"thd-source",
      "TranscodingUrl":"/stream.ts?AudioCodec=truehd&VideoCodec=hevc",
      "MediaStreams":[{"Type":"Video","Codec":"hevc"},
      {"Type":"Audio","Index":1,"Codec":"truehd","Channels":8,"SampleRate":48000}]}]})";
    PlaybackRequest truehd;
    truehd.item_id = "thd";
    truehd.audio_index = 1;
    truehd.allow_bitstream = true;
    truehd.allow_software_audio = true;
    assert(playback_plan(truehd).valid);
    auto truehd_profile = slopfin::json::parse(bodies.back());
    assert(truehd_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("AudioCodec") == "truehd,ac3,aac");
    assert(truehd_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("MaxAudioChannels") == "8");
    // Turning the CPU decoder off preserves server TrueHD -> E-AC-3 for HDMI.
    truehd.allow_software_audio = false;
    assert(playback_plan(truehd).valid);
    truehd_profile = slopfin::json::parse(bodies.back());
    assert(truehd_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("AudioCodec") == "eac3");
    assert(truehd_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("MaxAudioChannels") == "6");
    truehd.allow_software_audio = true;
    // With no bitstream port, the console's own decoder is still the fallback.
    truehd.allow_bitstream = false;
    assert(playback_plan(truehd).valid);
    truehd_profile = slopfin::json::parse(bodies.back());
    assert(truehd_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("AudioCodec") == "truehd,ac3,aac");

    // A format chosen in the Audio panel converts to exactly that, capped at 5.1 for Dolby.
    response_body = R"({"PlaySessionId":"fmt","MediaSources":[{"Id":"fmt-source",
      "TranscodingUrl":"/stream.ts?AudioCodec=eac3&VideoCodec=hevc",
      "MediaStreams":[{"Type":"Video","Codec":"hevc"},
      {"Type":"Audio","Index":1,"Codec":"truehd","Channels":8,"SampleRate":48000}]}]})";
    PlaybackRequest format;
    format.item_id = "fmt";
    format.audio_index = 1;
    format.audio_codec = "eac3";
    format.allow_software_audio = true;
    format.allow_bitstream = true;
    assert(playback_plan(format).valid);
    auto format_profile = slopfin::json::parse(bodies.back());
    assert(format_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("AudioCodec") == "eac3");
    assert(format_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("MaxAudioChannels") == "6");
    assert(format_profile->find("DeviceProfile")
               ->find("DirectPlayProfiles")
               ->at(0)
               ->str("AudioCodec") == "eac3");
    format.audio_codec = "aac";
    assert(playback_plan(format).valid);
    format_profile = slopfin::json::parse(bodies.back());
    assert(format_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("AudioCodec") == "aac");
    assert(format_profile->find("DeviceProfile")
               ->find("TranscodingProfiles")
               ->at(0)
               ->str("MaxAudioChannels") == "8");
    response_body = baseline_response;
    const auto *codecs = profile->find("DeviceProfile")->find("CodecProfiles");
    bool rate_limited = false;
    for (std::size_t i = 0; i < codecs->size(); ++i)
        if (codecs->at(i)->str("Codec") == "aac")
        {
            const auto *conditions = codecs->at(i)->find("Conditions");
            for (std::size_t j = 0; j < conditions->size(); ++j)
                if (conditions->at(j)->str("Property") == "AudioSampleRate")
                    rate_limited = conditions->at(j)->str("Value") == "48000" &&
                                   conditions->at(j)->flag("IsRequired");
        }
    assert(rate_limited); // A 44.1 kHz AAC source must not copy into the 48 kHz sink.

    session_body =
        R"([{"DeviceId":"test","TranscodingInfo":{"VideoCodec":"h264","AudioCodec":"ac3","IsVideoDirect":false,"IsAudioDirect":false}}])";
    playback_delivery(plan);
    assert(plan.video_delivery_known && !plan.hdr && plan.bit_depth == 8);
    plan = playback_plan(request);
    session_body =
        R"([{"DeviceId":"test","TranscodingInfo":{"VideoCodec":"hevc","AudioCodec":"ac3","IsVideoDirect":true}}])";
    playback_delivery(plan);
    assert(plan.video_delivery_known && plan.hdr && plan.bit_depth == 10);
    plan = playback_plan(request);
    session_body =
        R"([{"DeviceId":"test","TranscodingInfo":{"VideoCodec":"hevc","AudioCodec":"aac","IsVideoDirect":true}}])";
    playback_delivery(plan);
    assert(!plan.video_delivery_known); // Previous stream's AAC status cannot describe AC-3.
    assert(query_value("/stream?AuDiOcOdEc=ac3%2Caac", "audiocodec") == "ac3,aac");
    PlaybackRequest defaults;
    defaults.item_id = "item";
    response_body = R"({"MediaSources":[{"Id":"source","DefaultAudioStreamIndex":2,
      "TranscodingUrl":"/stream.ts?AudioCodec=ac3",
      "MediaStreams":[{"Type":"Video","Codec":"hevc"},
       {"Type":"Audio","Index":1,"Codec":"aac","Channels":2},
       {"Type":"Audio","Index":2,"Codec":"truehd","Channels":8}]}]})";
    auto default_plan = playback_plan(defaults);
    assert(default_plan.audio_codec == "truehd");
    defaults.audio_index = 1;
    assert(playback_plan(defaults).audio_codec == "aac");
    response_body = R"({"MediaSources":[{"Id":"source","DefaultAudioStreamIndex":2,
      "TranscodingUrl":"/stream.ts?AudioCodec=aac&allowaudiostreamcopy=true&AudioSampleRate=44100",
      "MediaStreams":[{"Type":"Video","Codec":"h264"},
       {"Type":"Audio","Index":1,"Codec":"aac","Channels":2,"SampleRate":48000},
       {"Type":"Audio","Index":2,"Codec":"aac","Channels":2,"SampleRate":44100}]}]})";
    defaults.audio_index = -1;
    auto stereo = playback_plan(defaults);
    assert(stereo.audio_sample_rate == 44100);
    assert(query_value(stereo.path, "AllowAudioStreamCopy") == "false");
    assert(query_value(stereo.path, "AudioSampleRate") == "48000");
    assert(stereo.path.find("allowaudiostreamcopy=true") == std::string::npos);
    session_body =
        R"([{"DeviceId":"test","TranscodingInfo":{"VideoCodec":"h264","AudioCodec":"aac","IsVideoDirect":true,"IsAudioDirect":true}}])";
    playback_delivery(stereo);
    assert(!stereo.video_delivery_known); // A stale AAC-copy session cannot confirm resampling.
    defaults.audio_index = 1;
    auto native_stereo = playback_plan(defaults);
    assert(native_stereo.audio_sample_rate == 48000);
    assert(query_value(native_stereo.path, "AllowAudioStreamCopy") == "true");
    assert(with_query_value("/s?X=1&x=2&Keep=%26#f", "X", "3") == "/s?Keep=%26&X=3#f");
    response_body = R"({"MediaSources":[{"Id":"source","TranscodingUrl":"/stream.ts?AudioCodec=ac3",
      "MediaStreams":[{"Type":"Video","Codec":"hevc","BitDepth":10,"VideoRangeType":"DOVIWithEL",
       "DvProfile":7,"DvBlSignalCompatibilityId":6,"ColorTransfer":"smpte2084","ColorPrimaries":"bt2020"},
       {"Type":"Audio","Index":1,"Codec":"ac3","Channels":6,"SampleRate":48000}]}]})";
    assert(!playback_plan(defaults).dv_hdr10_base);
    defaults.allow_dv_hdr10_base = true;
    auto dv = playback_plan(defaults);
    assert(dv.dv_hdr10_base && dv.hdr);
    assert(bodies.back().find("SDR|HDR10|DOVIWithEL") != std::string::npos);
    assert(bodies.back().find("DOVIWithHLG") == std::string::npos);
    assert(requests.back().find("MediaSourceId=source") != std::string::npos);
    session_body =
        R"([{"DeviceId":"test","TranscodingInfo":{"VideoCodec":"hevc","AudioCodec":"ac3","IsVideoDirect":false,"IsAudioDirect":true}}])";
    playback_delivery(dv);
    assert(dv.video_delivery_known && !dv.hdr && !dv.dv_hdr10_base && dv.bit_depth == 8);
    defaults.media_source_id = "absent-version";
    assert(!playback_plan(defaults).valid);
    response_body.clear();
    assert(!playback_plan(request).valid); // Never a hidden fixed-rate fallback.
    std::puts("Negotiation quality, TrueHD fallback and delivered HDR tests passed");
}
