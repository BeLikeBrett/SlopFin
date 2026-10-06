# Profile and server dashboard

Square opens the profile menu on Home and library pages. You can also reach it
through **Settings → Account**. The menu includes Profile, Dashboard for
administrator accounts, and Sign Out.

## Profile pictures

Change picture opens a gallery of JPEG/PNG images from PS5 captures and USB
storage. Gallery access needs elfldr running on port 9021; the app uses its
bundled helper to access those folders.

**Adjust your picture** opens the circular editor:

| Control | Action |
| --- | --- |
| D-pad | Move the circular crop |
| L1/R1 or triggers | Zoom |
| Triangle | Reset |
| Cross | Save |
| Circle | Return without saving |

A second circle previews the avatar. Saving uploads the selected region to
Jellyfin as a JPEG of at most 512×512 pixels.

![Circular avatar editor on PS5](images/avatar-crop-console.png)

Changing or removing a picture and changing a password affect your Jellyfin
account, including its appearance in other clients.

## Dashboard

Administrator accounts can inspect the server from the PS5: current sessions,
recent activity, devices, libraries, users, scheduled tasks, playback settings,
general settings, logs and plugins.

Available actions include pausing/stopping sessions, scanning libraries,
running tasks, managing devices/users/plugins, saving settings, and restarting
or shutting down the server. Actions that change server state require
confirmation; irreversible actions use a hold-to-confirm control.

These actions use your Jellyfin permissions. Read-only views and action menus
have been checked on the console; destructive actions have not been exercised
against the production server.
