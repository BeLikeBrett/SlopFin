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
| Square | Switch Browse / My requests; shown beside the destination tab |
| L1 / R1 | Previous / next catalog page |
| Circle | Close the title or return to the sidebar |

Availability badges come from the request server. Processing can include
waiting for a download; it does not mean bytes are currently arriving.
Choose Movies & series, Movies or Series beside the search field. The adjacent
sort control cycles Trending, Popular, Top rated, Newest and Upcoming. Changes
apply immediately. These catalogs use the server's discovery endpoints and
support page navigation with L1/R1; search uses the server's relevance order.
Choosing a catalog control clears the search. Top rated requires at least 200
votes; Newest excludes future dates and requires 10 votes to reduce empty entries.
When browsing both types, SlopFin combines the same page of each catalog.
My requests has no filters.

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
  selected again. **Triangle** or Right opens the focused season’s episode guide,
  including available and pending seasons. Episode entries are read-only;
  requests are made by season. Specials can be selected separately. Square
  selects missing regular seasons or clears the selection. If the server disables partial
  requests, SlopFin follows its all-missing-seasons policy.

**Options** focuses the request button. Review the selected profile and season
count, then press Cross to submit. Circle closes without requesting anything.
Server quotas, approval rules and account permissions still apply.

Available titles with a Jellyfin item ID offer **Open in Jellyfin**. Circle
returns to the same Discover view. The Jellyfin server you use in SlopFin must
contain that item and allow your account to access it.

## Follow downloads

My requests loads the signed-in account's requests, newest first (up to 2,000
entries). Up/Down selects a request; Triangle refreshes immediately.
Season counts, 4K and quality-profile names appear when supplied by the server.

The list refreshes about every five seconds during downloads, or every 30 seconds
otherwise, without moving your selection.
Active download entries show progress, remaining bytes, a measured speed and
the server's ETA when available. Speed appears after a second sample. Names
and artwork fill in as detail lookups finish. Progress depends on the request
server's Sonarr/Radarr and downloader integration.

Open a request and choose **Cancel request** to review removal. Seerr allows
ordinary users to remove their own pending requests; administrators and request
managers can remove other request states. The confirmation explains that library
files stay and downloads already sent to Sonarr/Radarr may continue. The server
makes the final permission decision.

The address, API key or session is saved in the console's protected
`/data/slopfin/config.json`. Passwords are used only for sign-in and are not
saved. Discover does not install or configure Sonarr, Radarr or a downloader.
