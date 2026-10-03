# Logs and bug reports

For a playback problem, include your PS5 firmware and loader, Jellyfin version,
media video/audio formats, selected playback settings, and what happened.
A short reproduction is more useful than a large unsorted log.

**Settings → Diagnostics** shows local report information. Reports stay on the
console unless you configure a receiver and choose Send. They can contain media
names, device/build details and trace excerpts; review them before sharing.

Useful files accessible through your console's FTP server:

| File | Purpose |
| --- | --- |
| `/data/slopfin-trace.txt` | Startup and playback events, including the build stamp |
| `/data/slopfin-session.txt` | Current session state |
| `/data/slopfin-crash.txt` | Latest captured crash report |

A leftover session can result from PS5 Close Game or a power interruption; it
is not proof of a crash. Remove credentials and private addresses before posting
logs in a [bug report](https://github.com/BeLikeBrett/SlopFin/issues/new/choose).

Developers can use the [diagnostic internals](development/DIAGNOSTICS.md) and
[testing tools](development/TESTING.md) to reproduce a fault or record playback.
