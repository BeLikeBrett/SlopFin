# Updating SlopFin

**Status:** the updater is in draft preview 01.000.002 and still needs its final
console download/install test. Published previews currently use manual folder
installation. The instructions below describe the upcoming updater.

Folder builds have **Settings → Updates**. Check for a release, choose
**Download update**, then **Install and restart** after verification finishes.
The app closes before installation and opens again afterwards. Keep the console
on until it returns.

The console must be jailbroken with its payload loader **elfldr on port 9021**
running. SlopFin sends its bundled installer helper to the local console; it
does not require a development computer. If the helper cannot start, the app
stays open and reports the problem.

This feature first ships in **01.000.002**. Earlier releases need a one-time
manual folder upgrade. Extract the complete folder download, close SlopFin,
and replace `/data/homebrew/PPSA99001` through your loader's normal installation
method. Keep `/data/slopfin/config.json`; it contains your sign-in and settings.
Future published folder releases can be installed from inside the app.

## Release source and verification

The source is [BeLikeBrett/SlopFin releases](https://github.com/BeLikeBrett/SlopFin/releases).
The checker includes public preview releases and displays their Preview label.
It accepts a higher version in the PS5 `NN.NNN.NNN` format, the matching folder
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

## Recovery and native packages

The previous files are kept at `/data/slopfin/update/backup`. If SlopFin cannot
open after an interrupted installation, close it and reinstall the complete
release folder through your usual loader, or restore the backed-up files.
If an interrupted helper left `/data/slopfin/update/install.lock`, remove that
file only after SlopFin is closed and the console has been restarted.
Do not copy files over a running app. A power interruption during installation
can require this manual recovery; the helper cannot run while the console is off.

Native FPKG installations require their package installer. The folder updater
refuses to replace a folder that does not match the running app. Native package
mounting remains experimental on the test console; see
[native package status](development/NATIVE_FPKG.md).
