/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef SLOPFIN_SUBTITLE_LABELS_HPP
#define SLOPFIN_SUBTITLE_LABELS_HPP

#include "jellyfin.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace slopfin::subtitle_labels
{
inline std::string lower(std::string_view value)
{
    std::string out{value};
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/* ISO 639 two/three-letter codes, including the bibliographic aliases used
   in older MKVs. Unrecognised tags stay visible rather than guessing. */
inline std::string language(std::string_view tag)
{
    struct Language
    {
        const char *codes;
        const char *name;
    };
    constexpr Language languages[] = {
        {"|en|eng|", "English"},
        {"|es|spa|", "Spanish"},
        {"|fr|fra|fre|", "French"},
        {"|de|deu|ger|", "German"},
        {"|it|ita|", "Italian"},
        {"|pt|por|", "Portuguese"},
        {"|ja|jpn|", "Japanese"},
        {"|ko|kor|", "Korean"},
        {"|zh|zho|chi|", "Chinese"},
        {"|ar|ara|", "Arabic"},
        {"|ru|rus|", "Russian"},
        {"|nl|nld|dut|", "Dutch"},
        {"|sv|swe|", "Swedish"},
        {"|no|nor|", "Norwegian"},
        {"|nb|nob|", "Norwegian Bokmål"},
        {"|nn|nno|", "Norwegian Nynorsk"},
        {"|da|dan|", "Danish"},
        {"|fi|fin|", "Finnish"},
        {"|is|isl|ice|", "Icelandic"},
        {"|pl|pol|", "Polish"},
        {"|cs|ces|cze|", "Czech"},
        {"|sk|slk|slo|", "Slovak"},
        {"|hu|hun|", "Hungarian"},
        {"|ro|ron|rum|", "Romanian"},
        {"|bg|bul|", "Bulgarian"},
        {"|el|ell|gre|", "Greek"},
        {"|tr|tur|", "Turkish"},
        {"|uk|ukr|", "Ukrainian"},
        {"|he|heb|iw|", "Hebrew"},
        {"|hi|hin|", "Hindi"},
        {"|th|tha|", "Thai"},
        {"|vi|vie|", "Vietnamese"},
        {"|id|ind|in|", "Indonesian"},
        {"|ms|msa|may|", "Malay"},
        {"|ta|tam|", "Tamil"},
        {"|te|tel|", "Telugu"},
        {"|ml|mal|", "Malayalam"},
        {"|bn|ben|", "Bengali"},
        {"|ur|urd|", "Urdu"},
        {"|fa|fas|per|", "Persian"},
        {"|hr|hrv|", "Croatian"},
        {"|sr|srp|", "Serbian"},
        {"|bs|bos|", "Bosnian"},
        {"|sl|slv|", "Slovenian"},
        {"|et|est|", "Estonian"},
        {"|lv|lav|", "Latvian"},
        {"|lt|lit|", "Lithuanian"},
        {"|ca|cat|", "Catalan"},
        {"|eu|eus|baq|", "Basque"},
        {"|gl|glg|", "Galician"},
        {"|af|afr|", "Afrikaans"},
        {"|sw|swa|", "Swahili"},
        {"|fil|", "Filipino"},
        {"|tl|tgl|", "Tagalog"},
        {"|yue|", "Cantonese"},
        {"|cmn|", "Mandarin"},
        {"|mk|mkd|mac|", "Macedonian"},
        {"|sq|sqi|alb|", "Albanian"},
        {"|hy|hye|arm|", "Armenian"},
        {"|ka|kat|geo|", "Georgian"},
        {"|az|aze|", "Azerbaijani"},
        {"|kk|kaz|", "Kazakh"},
        {"|uz|uzb|", "Uzbek"},
        {"|mn|mon|", "Mongolian"},
        {"|ne|nep|", "Nepali"},
        {"|si|sin|", "Sinhala"},
        {"|my|mya|bur|", "Burmese"},
        {"|km|khm|", "Khmer"},
        {"|lo|lao|", "Lao"},
        {"|cy|cym|wel|", "Welsh"},
        {"|ga|gle|", "Irish"},
        {"|mt|mlt|", "Maltese"},
        {"|la|lat|", "Latin"},
        {"|eo|epo|", "Esperanto"},
    };
    std::string code = lower(tag);
    std::replace(code.begin(), code.end(), '_', '-');
    const auto separator = code.find('-');
    const std::string base = code.substr(0, separator);
    if (base.empty() || base == "und" || base == "unk")
        return {};
    if (base == "zxx")
        return "No dialogue";
    if (base == "mul")
        return "Multiple languages";
    const std::string needle = "|" + base + "|";
    for (const auto &entry : languages)
        if (std::string_view{entry.codes}.find(needle) != std::string_view::npos)
            return std::string{entry.name} +
                   (separator == std::string::npos
                        ? ""
                        : " (" + std::string{tag.substr(separator + 1)} + ")");
    return std::string{tag};
}

inline std::string label(const jellyfin::Track &track)
{
    const auto codec = lower(track.codec);
    const std::string format =
        codec == "pgsub" || codec == "pgssub" || codec == "hdmv_pgs_subtitle" ? "PGS"
        : codec == "dvdsub" || codec == "dvd_subtitle" || codec == "vobsub"   ? "DVD"
        : codec == "subrip" || codec == "srt"                                 ? "SRT"
        : codec == "ass"                                                      ? "ASS"
        : codec == "ssa"                                                      ? "SSA"
        : codec == "mov_text"                                                 ? "Text"
                                                                              : track.codec;
    const auto name = language(track.language);
    std::string description = track.label;
    // Normalise the format token without changing language/variant details.
    for (const auto &[from, to] :
         {std::pair{"pgssub", "PGS"}, std::pair{"pgsub", "PGS"}, std::pair{"subrip", "SRT"}})
    {
        const auto at = lower(description).find(from);
        if (at != std::string::npos)
            description.replace(at, std::string_view{from}.size(), to);
    }
    const auto generic = lower(description);
    if (generic == codec || generic == lower(format) || generic == "pgsub" || generic == "pgssub")
        description.clear();
    std::string out = name;
    const auto add = [&](const std::string &part)
    {
        if (!part.empty())
            out += (out.empty() ? "" : " · ") + part;
    };
    // A rich server title (English - PGS - Forced, commentary, etc.) is retained.
    if (!name.empty() && lower(description).rfind(lower(name), 0) == 0)
        out = description;
    else
        add(description);
    if (out.empty())
        out = "Unknown language";
    if (!format.empty() && lower(out).find(lower(format)) == std::string::npos)
        add(format);
    if (track.is_forced && lower(out).find("forced") == std::string::npos)
        add("Forced");
    if (track.is_hearing_impaired && lower(out).find("sdh") == std::string::npos &&
        lower(out).find("hearing impaired") == std::string::npos)
        add("SDH");
    if (track.is_default && lower(out).find("default") == std::string::npos)
        add("Default");
    return out;
}
} // namespace slopfin::subtitle_labels
#endif
