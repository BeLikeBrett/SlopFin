# Updating SlopFin

Folder builds have **Settings → Updates**. Check for a release, choose
**Download update**, then **Install and restart** after verification finishes.
The app closes before installation and opens again afterwards. Keep the console
on until it returns.

The console must be jailbroken with its payload loader **elfldr on port 9021**
running. SlopFin sends its bundled installer helper to the local console; it
does not require a development computer. If the helper cannot start, the app
stays open and reports the problem. Localhost-only loaders work; port 9021
does not need to be accessible from another device.

If update storage is unavailable, check whether the running app can access
`/data`. A successful FTP connection does not establish application access.
Missing paths, permission failures and full storage are reported separately.

Build **1.12** includes an update-storage helper. If preparing the download
fails because the launcher restricts app data, SlopFin asks elfldr to restore
access and retries. This recovery runs from **Download update**, after the
interface and account have loaded. It verifies the running executable and
metadata against the folder install before changing the process root, then
uses the verified folder path for installer files. It creates no extra mount.

An older installed updater cannot use this helper until 1.12 is installed.
If 1.10 or 1.11 still reports a storage error, install the complete 1.12 folder
once using the instructions below. Keep `/data/slopfin/config.json`.

Build **1.08** fixes GitHub downloads and installer file permissions. **1.09** also
replaces stale staging and handoff files left by older installers or FTP tools. Versions **1.07 and earlier**
need a one-time manual folder upgrade. Extract the complete folder download, close SlopFin,
and replace `/data/homebrew/PPSA99001` through your loader's normal installation
method. Keep `/data/slopfin/config.json`; it contains your sign-in and settings.
Future published folder releases can be installed from inside the app.

## Release source and verification

The source is [BeLikeBrett/SlopFin releases](https://github.com/BeLikeBrett/SlopFin/releases).
The checker includes public preview releases and displays their Preview label.
Versions are displayed as **1.08**. Internally the PS5 requires `01.000.008`;
new GitHub tags and downloads use the short label. The checker accepts both tag
styles, compares the full version and requires the matching folder
ZIP name, and GitHub's SHA-256 asset digest. Builds without a valid version or
releases missing a digest are not update candidates.

Downloads use verified HTTPS. Only GitHub and its release asset hosts can
receive download redirects; Jellyfin credentials are never sent to them.
The archive is checked against the digest before extraction. Extraction
validates paths, CRCs, file sizes, title ID and version. Encrypted files,
symlinks, duplicate paths, traversal and unexpected application paths are
rejected. Compressed downloads are capped at 32 MiB, extracted contents at
128 MiB and the archive file count at 256.

The download and staged files live under `/data/slopfin/update`. The helper
checks that the running app matches the installed folder, verifies staged
files again and backs up the previous files before asking the app to close.
It replaces individual files atomically, leaving the executable and version
metadata until last. An installation error restores files already replaced.
Sign-in, preferences, subtitle offsets and per-series autoplay settings live
outside the application folder and are not replaced.

The complete GitHub download/install/restart flow was checked on firmware 8.20
with 1.08 and 1.09. The latter also recovered staging and handoff files owned
by an older FTP installation; the protected account configuration stayed unchanged.

## Recovery

The previous files are kept at `/data/slopfin/update/backup`. If SlopFin cannot
open after an interrupted installation, close it and reinstall the complete
release folder through your usual loader, or restore the backed-up files.
If an interrupted helper left `/data/slopfin/update/install.lock`, remove that
file only after SlopFin is closed and the console has been restarted.
Do not copy files over a running app. A power interruption during installation
can require this manual recovery; the helper cannot run while the console is off.

The updater verifies that the installed folder matches the running app before
replacing files. Install the complete folder release if this check fails.

## Download errors

Interrupted downloads retry automatically. The app reports network, wrong-size,
permission and full-storage failures separately. A failed download leaves the
installed app unchanged; choose Download update again to retry. If staging or
backup validation fails, download a fresh archive. Diagnostic logs record the
failed stage and byte count without request credentials.
