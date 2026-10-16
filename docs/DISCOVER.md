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
| Square or L1/R1 | Switch Browse / My requests; Square is shown beside the destination tab |
| R3 | Open media and status filters |
| Circle | Close the title or return to the sidebar |

Availability badges come from the request server. Processing can include
waiting for a download; it does not mean bytes are currently arriving.
Filters select Movies, Series or both. Browse also filters by availability;
My requests filters by downloading, processing, available, awaiting approval or
declined. Choose values with Left/Right and apply them. Circle cancels without
changing the current filters. Filters act on the loaded results (the first
search/trending page or the latest 60 requests).

## Seasons and quality

Open a title to review its request options before sending anything:

- **Quality profile:** Left/Right cycles the profiles provided by Sonarr or
  Radarr. The configured default is selected initially. Accounts need Seerr's
  advanced-request or request-management permission to override it; other
  accounts use the server default and see an explanation.
- **Download server:** choose between configured services when your account
  permits it. A **Standard / 4K** control appears when a 4K service is configured
  and your account can request 4K. Profiles such as Ultra-HD can also exist on
  an ordinary service; profile names come from your server.
- **Seasons:** Up/Down moves through the list; Cross toggles an individual
  missing season. Available and pending seasons are labelled and cannot be
  selected again. Specials can be selected separately. Square selects missing
  regular seasons or clears the selection. If the server disables partial
  requests, SlopFin follows its all-missing-seasons policy.

**Options** focuses the request button. Review the selected profile and season
count, then press Cross to submit. Circle closes without requesting anything.
Server quotas, approval rules and account permissions still apply.

Available titles with a Jellyfin item ID offer **Open in Jellyfin**. Circle
returns to the same Discover view. The Jellyfin server you use in SlopFin must
contain that item and allow your account to access it.

## Follow downloads

My requests shows the signed-in request account's latest 60 requests, newest
first. Up/Down selects a request; R3 opens filters. Triangle refreshes immediately.
Season counts, 4K and quality-profile names appear when supplied by the server.

The list refreshes about every five seconds without moving your selection.
Active download entries show progress, remaining bytes, a measured speed and
the server's ETA when available. Speed appears after a second sample. Names
and artwork fill in as detail lookups finish. Progress depends on the request
server's Sonarr/Radarr and downloader integration.

The address, API key or session is saved in the console's protected
`/data/slopfin/config.json`. Passwords are used only for sign-in and are not
saved. Discover does not install or configure Sonarr, Radarr or a downloader.
