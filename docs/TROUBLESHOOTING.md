# Troubleshooting

## SlopFin will not start

Install the complete folder download, including `sce_module`, `sce_sys` and
`assets`; copying only `eboot.bin` is insufficient. Check your loader supports
app folders and registered PPSA99001. Close any old instance before upgrading.

Native FPKG launch is currently blocked during package mounting on the test
console. Use the folder build. [Installation](GETTING_STARTED.md).

## My server will not connect

Check the address from another device on the same network. A hostname defaults
to HTTPS, while a bare IPv4 address defaults to HTTP/8096. Include an explicit
protocol/port for a different setup and include any server base path.
Self-signed certificates and IPv6 are unsupported; HTTPS will not silently
switch to HTTP. [Address examples](GETTING_STARTED.md#enter-your-server).

## There is no Skip Intro button

The episode needs an Intro segment or a valid named intro chapter. Configure a
server segment provider, run its scan, and reopen the episode after markers are
generated. [Intro setup](INTRO_SKIPPING.md).

## Playback buffers, has no sound, or looks wrong

Try a lower playback quality and a compatible audio track. Jellyfin may need
to transcode an unsupported source, and server storage or delivery stalls can
cause buffering even with a fast connection. Check Jellyfin's active session
for its actual copy/transcode path.

HDR10 output, HDMI passthrough and the optional software-audio decoder have
separate experimental limits. [Compatibility](COMPATIBILITY.md) explains those
paths; test ordinary PCM/SDR playback before diagnosing an experimental output.

## Picture selection or updates cannot open

The console's elfldr must be running on port 9021 for the helper to start.
Folder preview 01.000.003 includes Settings → Updates. Final console install/restart
verification is pending for this build. The complete folder ZIP also supports manual installation. [Updates and recovery](UPDATES.md).

If a problem persists, attach a short reproduction and a redacted
[diagnostic report](DIAGNOSTICS.md). For compiler or package-tool issues, use
[build troubleshooting](development/BUILD_TROUBLESHOOTING.md).
