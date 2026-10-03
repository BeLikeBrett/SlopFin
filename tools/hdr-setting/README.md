# Console HDR setting (dev tooling)

*Opus 5, 2026-09-15.* Payloads sent to elfldr (port 9021).

| Payload | What it does |
| --- | --- |
| `videoout-read.elf` | Writes every VIDEOOUT registry entry, with its factory value, to `/data/slopfin-videoout-registry.txt` |
| `videoout-set-hdr-0.elf` | Sets Settings > Screen and Video > HDR to **On When Supported** (0, the factory value) |
| `videoout-set-hdr-1.elf` | Sets it to 1, read as **Off** (see below) |
| `avsetting.elf` | Probes libSceAvSetting; its monitor queries answer `0x802a0002` from a payload and `sceAvSettingInit` never returns |

```
make -C tools/hdr-setting PS5_PAYLOAD_SDK=$PWD/.deps/native/ps5-payload-sdk videoout-read.elf videoout-set-hdr-0.elf videoout-set-hdr-1.elf
```

The console applies a write immediately: `sceVideoOutGetOutputStatus` byte 4 in
SlopFin's display report went 2 -> 1 -> 2 with writes of 0, 1, 0, and SlopFin's
next title followed it without a restart. Key numbers are from
ps5-payload-dev/linkdev `regmgr.h`. The label for 1 is inferred, not read: 0 is the factory value and the PS5 ships
with HDR "On When Supported", the menu has only two choices, and the Screen and
Video menu refuses to open during Remote Play, so the menu could not be looked at
with 1 written. Leave the console on 0 unless Brett asks otherwise.
