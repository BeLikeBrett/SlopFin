# Discover and requests

Discover sits above Home in the sidebar. Connect Seerr or Jellyseerr to browse
trending titles, search for a movie or series, request it and follow its download.
An Overseerr-compatible server can use API-key or local-account authentication.

## Connect

Choose **Connect request server** in an unconfigured Discover screen, or open
**Settings → Requests**.

1. Enter the server address. `request.example.com` uses verified HTTPS;
   a bare local IP uses HTTP on port 5055. Explicit protocols, ports and base
   paths are supported.
2. Choose **Connect using**: Jellyfin account, request-server account, or API key.
3. Enter the fields shown for that method, then choose **Connect**.

API keys come from the request server's Settings → General. They act as its
owner, so requests and My requests use that account. Use account sign-in when
you want requests credited to your own user. Keys can be revoked or rotated
on the server. Disconnect removes saved request credentials and keeps your
Jellyfin sign-in.

## Browse and request

| Control | Action |
| --- | --- |
| Triangle | Search using the PlayStation keyboard |
| D-pad | Browse titles |
| Cross | Open a title, then choose its action |
| Square or L1/R1 | Switch Search / My requests |
| Circle | Close the title or return to the sidebar |

Availability badges come from the request server. Processing can include
waiting for a download; it does not mean bytes are currently arriving.
Requesting a series asks for all seasons, including missing seasons of a
partly available show. There is no individual season picker.

Available titles with a Jellyfin item ID offer **Open in Jellyfin**. Circle
returns to the same Discover view. The Jellyfin server you use in SlopFin must
contain that item and allow your account to access it.

## Follow downloads

My requests shows the signed-in request account's latest 60 requests, newest
first. Up/Down selects a request; Left/Right filters All, Downloading,
Available or Awaiting approval. Triangle refreshes immediately.

The list refreshes about every five seconds without moving your selection.
Active download entries show progress, remaining bytes, a measured speed and
the server's ETA when available. Speed appears after a second sample. Names
and artwork fill in as detail lookups finish. Progress depends on the request
server's Sonarr/Radarr and downloader integration.

The address, API key or session is saved in the console's protected
`/data/slopfin/config.json`. Passwords are used only for sign-in and are not
saved. Discover does not install or configure Sonarr, Radarr or a downloader.
