- **More of your library plays on the PS5 itself.** Two settings that were off
  by default are now on for everyone, including existing installs:
  - **Decode TrueHD / DTS / E-AC-3 on PS5**: these soundtracks, common on 4K
    and Blu-ray rips, are decoded by the console instead of being converted by
    your Jellyfin server, so those files can play directly.
  - **Dolby Vision HDR10 fallback**: Dolby Vision profile 7 and 8.1 files play
    from their HDR10 layer on the PS5, instead of your server converting the
    whole 4K picture.
- **Automatic fallback.** If either one fails on a title, SlopFin restarts it
  where it stopped and lets Jellyfin convert that part, instead of stopping
  with an error. It stays that way for the rest of that title.
- You can still turn either setting off in **Settings → Audio/Video**; that
  choice is kept from now on.

Update in **Settings → Updates**, or install the complete **folder ZIP** below.
Sign-in and preferences are preserved. [Update guide](https://github.com/BeLikeBrett/SlopFin/blob/main/docs/UPDATES.md).
