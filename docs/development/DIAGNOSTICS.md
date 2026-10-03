# Diagnostics: crashes, hangs, and sending a report

## What the app writes

| File on the console | What it is |
| --- | --- |
| `/data/slopfin-session.txt` | what this run is doing, rewritten when the state changes -- never on a timer, because a periodic write from the render thread costs vertical blanks |
| `/data/slopfin-crash.txt` | the newest fatal fault, written from the signal handler |
| `/data/slopfin-crashes.log` | every report this console has produced, appended |
| `/data/slopfin-lastrun.txt` | the session file of a run that never shut down, kept at the next start |
| `/data/slopfin-report-sent.txt` | the fingerprint of the report already sent |
| `/data/slopfin-report-offered.txt` | the fingerprint of the report already offered in the UI, so dismissing it does not nag on every launch |

A report carries: the build stamp, the module's load address, the signal and
the faulting address, `rip`/`rsp`/`rbp`, the frame count, the phrase for what
the app was doing (`playing The Batman`, `detail Interstellar`), the last dozen
notes, a frame-pointer backtrace, and the tail of the trace.

## Reading one

```
tools/crash.sh                 # newest report, the unfinished run, this session
tools/crash.sh --all           # the whole history
tools/crash.sh --clear         # delete them from the console
tools/crash-symbols.py report.txt [build/llvm-pie.elf]
```

`crash-symbols.py` turns addresses into functions and lines. It needs the ELF
from **the same build the report came from**: the report's `anchor` line gives
the runtime address of one known function, and the difference against that
function's address in the ELF is the load base. The console build keeps frame
pointers and line tables (`tools/build.sh`) for exactly this.

## Optional report uploads

Reports stay local by default. SlopFin does not assume that a Jellyfin server
also hosts a report receiver. To enable uploads, set `reportServer` in the
console's `/data/slopfin/config.json` to the root URL of your own receiver,
for example `https://reports.example.com` or `http://192.168.1.20:8103`.
The receiver must accept a text POST at `/report` (under any configured base
path). Jellyfin authentication headers are not sent to it.

With a valid receiver configured, a fatal report offers **Send report / Not now**
once after restart; Settings → Diagnostics also allows a manual upload.
Without a receiver, startup does not offer an upload, and Diagnostics explains
that reports remain local. The Dashboard's server-log upload action is available
only with a configured receiver. Existing `/data/slopfin-report-port` markers
are no longer used.

Nothing uploads without selecting Send. Reports contain media names, device
and build details, fault registers and trace excerpts; inspect them before
sharing. Dismissing the prompt keeps the local evidence. An unfinished session
alone is retained as diagnostic evidence rather than reported as a confirmed
crash, because PS5 Close Game can terminate the app without a catchable signal.

## Proving it works

- `make test-playback` runs `tests/test_crash_report.cpp`, which forks a child,
  makes it fault for real, and reads what the handler wrote -- including that
  the backtrace names a real caller and that only the tail of the trace is kept.
- On the console, writing `1` to `/data/slopfin-crash-test` makes the app fault
  on purpose (`2` abandons it, anything else aborts). That is how the report
  above was produced.

## Signal-context ABI

The console's signal context is **not** laid out where the SDK's
`x86/ucontext.h` says. Read as a `mcontext_t`, `mc_rsp` came back holding the
faulting address -- that is `mc_addr`, six slots earlier. The handler therefore
finds `mc_addr` by looking for the faulting address in the context block and
reads the registers at their fixed distances from it; the report prints which
word it found (`context mc_addr at word 25` on firmware 8.20).


## Orderly shutdown

The tooling quit marker asks the main render loop to stop. The loop closes
the IME dialog, stops playback, releases VideoOut, finishes the session and
requests process termination. If termination returns, the thread parks;
rendering must never resume against the released display.
