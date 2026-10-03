/*
 * SlopFin - diagnostics for a crash, a hang, or a session that vanished.
 * Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "crash.hpp"

#include "trace.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <exception>
#ifndef SLOPFIN_HOST
#include <sys/ucontext.h>
#endif
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <pthread.h>

#ifdef SLOPFIN_HOST
#include <fcntl.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* The preview has no Sony threading; the two calls it needs are these. */
static inline int scePthreadCreate(void **thread, const void *, void *(*entry)(void *),
                                   void *argument, const char *)
{
    pthread_t created{};
    const int result = pthread_create(&created, nullptr, entry, argument);
    if (result == 0 && thread != nullptr)
        *thread = reinterpret_cast<void *>(created);
    return result;
}
static inline int sceKernelUsleep(unsigned microseconds)
{
    return usleep(microseconds);
}
static inline int scePthreadSetaffinity(void *, std::uint64_t)
{
    return 0;
}
#else
extern "C"
{
    int scePthreadCreate(void **thread, const void *attr, void *(*entry)(void *), void *argument,
                         const char *name);
    int scePthreadSetaffinity(void *thread, std::uint64_t mask);
    int sceKernelUsleep(unsigned microseconds);
    int open(const char *path, int flags, ...);
    long write(int descriptor, const void *buffer, unsigned long bytes);
    long read(int descriptor, void *buffer, unsigned long bytes);
    int close(int descriptor);
    int unlink(const char *path);
    void _exit(int code);
}
#endif

namespace slopfin::crash
{
namespace
{
/* The console's kernel is FreeBSD's, whose open flags are not Linux's; the
   preview builds against the host's own header rather than these numbers. */
#ifdef SLOPFIN_HOST
constexpr int kWriteOnly = O_WRONLY;
constexpr int kReadOnly = O_RDONLY;
constexpr int kCreate = O_CREAT;
constexpr int kTruncate = O_TRUNC;
constexpr int kAppend = O_APPEND;
#else
constexpr int kWriteOnly = 0x0001;
constexpr int kReadOnly = 0x0000;
constexpr int kCreate = 0x0200;
constexpr int kTruncate = 0x0400;
constexpr int kAppend = 0x0008;
#endif

#ifdef SLOPFIN_HOST
constexpr const char *kSession = "slopfin-session.txt";
constexpr const char *kCrash = "slopfin-crash.txt";
constexpr const char *kHistory = "slopfin-crashes.log";
constexpr const char *kLastRun = "slopfin-lastrun.txt";
constexpr const char *kSentMark = "slopfin-report-sent.txt";
constexpr const char *kOfferedMark = "slopfin-report-offered.txt";
#else
constexpr const char *kSession = "/data/slopfin-session.txt";
constexpr const char *kCrash = "/data/slopfin-crash.txt";
constexpr const char *kHistory = "/data/slopfin-crashes.log";
constexpr const char *kLastRun = "/data/slopfin-lastrun.txt";
constexpr const char *kSentMark = "/data/slopfin-report-sent.txt";
constexpr const char *kOfferedMark = "/data/slopfin-report-offered.txt";
#endif

/*
 * Everything the report needs lives in fixed storage written by ordinary code
 * and read by a signal handler: no allocation, no locks, nothing that can
 * itself fault while the process is already on fire.
 */
constexpr std::size_t kPhraseBytes = 96;
constexpr std::size_t kNotes = 12;

struct Notes
{
    char text[kNotes][kPhraseBytes]{};
    int frame[kNotes]{};
    std::atomic<unsigned> written{0};
};

char g_phase[kPhraseBytes] = "starting";
char g_detail[kPhraseBytes] = "";
std::atomic<bool> g_dirty{true};
std::atomic<bool> g_beating{false};
Notes g_notes;
std::atomic<int> g_frames{0};
std::atomic<bool> g_reported{false};
bool g_installed = false;

void copy_phrase(char *destination, std::string_view what) noexcept
{
    const std::size_t length = what.size() < kPhraseBytes - 1 ? what.size() : kPhraseBytes - 1;
    std::memcpy(destination, what.data(), length);
    destination[length] = '\0';
}

/* ---- writing, without the C library ---- */

class Report
{
  public:
    explicit Report(const char *path, bool append) noexcept
        : fd_(open(path, kWriteOnly | kCreate | (append ? kAppend : kTruncate), 0666))
    {
    }
    ~Report() noexcept
    {
        flush();
        if (fd_ >= 0)
            (void)close(fd_);
    }
    [[nodiscard]] bool good() const noexcept
    {
        return fd_ >= 0;
    }

    Report &text(const char *value) noexcept
    {
        while (value != nullptr && *value != '\0')
            put(*value++);
        return *this;
    }
    Report &text(std::string_view value) noexcept
    {
        for (char c : value)
            put(c);
        return *this;
    }
    Report &number(long long value) noexcept
    {
        if (value < 0)
        {
            put('-');
            value = -value;
        }
        char digits[24];
        int at = 0;
        do
        {
            digits[at++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0 && at < 24);
        while (at > 0)
            put(digits[--at]);
        return *this;
    }
    Report &hex(std::uint64_t value, int width = 0) noexcept
    {
        char digits[16];
        int at = 0;
        do
        {
            const auto nibble = static_cast<unsigned>(value & 0xf);
            digits[at++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
            value >>= 4;
        } while (value != 0 && at < 16);
        for (int pad = at; pad < width; ++pad)
            put('0');
        while (at > 0)
            put(digits[--at]);
        return *this;
    }
    Report &line() noexcept
    {
        return text("\n");
    }

  private:
    void put(char c) noexcept
    {
        if (used_ == sizeof(buffer_))
            flush();
        buffer_[used_++] = c;
    }
    void flush() noexcept
    {
        if (fd_ >= 0 && used_ > 0)
            (void)write(fd_, buffer_, used_);
        used_ = 0;
    }

    int fd_;
    char buffer_[1024]{};
    std::size_t used_ = 0;
};

/* ---- what every report says ---- */

template <typename Sink> void write_state(Sink &out) noexcept
{
    out.text("build   " __DATE__ " " __TIME__).line();
    /*
     * Where this build landed in memory. The module is position-independent,
     * so an address in the report means nothing on its own; with the runtime
     * address of one known function, tools/crash.sh --symbols subtracts that
     * function's address in build/llvm-pie.elf and turns every address here
     * into a file and a line.
     */
    out.text("anchor  _ZN7slopfin5crash7installEv 0x")
        .hex(reinterpret_cast<std::uint64_t>(&slopfin::crash::install), 12)
        .line();
    out.text("frame   ").number(g_frames.load(std::memory_order_relaxed)).line();
    out.text("doing   ").text(g_phase).line();
    if (g_detail[0] != '\0')
        out.text("state   ").text(g_detail).line();
    const unsigned written = g_notes.written.load(std::memory_order_relaxed);
    const unsigned first = written > kNotes ? written - kNotes : 0;
    for (unsigned i = first; i < written; ++i)
    {
        const std::size_t slot = i % kNotes;
        out.text("note    ").number(g_notes.frame[slot]).text("  ").text(g_notes.text[slot]).line();
    }
}

/* The tail of the trace, so the report carries the run's own last words. */
template <typename Sink> void write_trace_tail(Sink &out, int lines) noexcept
{
#ifdef SLOPFIN_HOST
    const int fd = open("slopfin-trace.txt", kReadOnly);
#else
    const int fd = open("/data/slopfin-trace.txt", kReadOnly);
#endif
    if (fd < 0)
        return;
    /* A fixed window off the end: the trace of a long session is megabytes,
       and the handler must not allocate to read it. */
    static char tail[8192];
    std::size_t used = 0;
    for (;;)
    {
        const long got = read(fd, tail + used, sizeof(tail) - used);
        if (got <= 0)
            break;
        used += static_cast<std::size_t>(got);
        if (used == sizeof(tail))
        {
            /* Keep the last half and carry on, so the window ends at the end. */
            std::memmove(tail, tail + sizeof(tail) / 2, sizeof(tail) / 2);
            used = sizeof(tail) / 2;
        }
    }
    (void)close(fd);
    if (used == 0)
        return;
    std::size_t start = used;
    int seen = 0;
    while (start > 0 && seen <= lines)
    {
        --start;
        if (tail[start] == '\n')
            ++seen;
    }
    if (tail[start] == '\n')
        ++start;
    out.text("--- trace tail ---").line();
    out.text(std::string_view{tail + start, used - start});
    if (used > 0 && tail[used - 1] != '\n')
        out.line();
}

void write_session_file() noexcept
{
    Report out(kSession, false);
    if (!out.good())
        return;
    out.text("slopfin session, running").line();
    write_state(out);
}

/* ---- the fatal handler ---- */

const char *signal_name(int number) noexcept
{
    switch (number)
    {
    case SIGSEGV:
        return "SIGSEGV (bad memory access)";
    case SIGBUS:
        return "SIGBUS (misaligned or unmapped)";
    case SIGILL:
        return "SIGILL (bad instruction)";
    case SIGFPE:
        return "SIGFPE (arithmetic)";
    case SIGABRT:
        return "SIGABRT (abort, or a failed assertion)";
    case SIGTRAP:
        return "SIGTRAP";
    case SIGSYS:
        return "SIGSYS (bad system call)";
    default:
        return "signal";
    }
}

/*
 * Return addresses from the frame-pointer chain.
 *
 * The console build keeps frame pointers on purpose
 * (-fno-omit-frame-pointer in tools/build.sh) so this walk is possible at all. Every pointer is
 * range-checked before it is read: a handler that faults produces no report.
 */
template <typename Sink>
void write_backtrace(Sink &out, std::uint64_t rbp, std::uint64_t rsp) noexcept
{
    out.text("--- stack ---").line();
    std::uint64_t frame = rbp;
    for (int depth = 0; depth < 24; ++depth)
    {
        /* The chain runs upward from the stack pointer, eight-byte aligned.
           Anything else is a register that was not a frame pointer, and
           following it would fault inside the handler. */
        if (frame < rsp || frame > rsp + (64u << 20) || (frame & 7u) != 0)
        {
            if (depth == 0)
            {
                /*
                 * Some code -- the C library, mostly -- keeps no frame
                 * pointer, so a fault inside it leaves rbp holding something
                 * else. The raw words are still worth printing: the return
                 * address into SlopFin is among them, and it is recognisable
                 * by sitting near rip.
                 */
                out.text("  no frame chain; stack words from rsp:").line();
                for (int i = 0; i < 24; ++i)
                {
                    const auto *word = reinterpret_cast<const std::uint64_t *>(rsp) + i;
                    out.text("  +").number(i * 8).text("  0x").hex(*word, 12).line();
                }
            }
            break;
        }
        const auto *slots = reinterpret_cast<const std::uint64_t *>(frame);
        const std::uint64_t next = slots[0];
        const std::uint64_t ret = slots[1];
        if (ret == 0)
            break;
        out.text("  ").number(depth).text("  0x").hex(ret, 12).line();
        if (next <= frame)
            break;
        frame = next;
    }
}

void write_fault(int number, siginfo_t *info, void *context) noexcept
{
    const auto address = reinterpret_cast<std::uint64_t>(info != nullptr ? info->si_addr : nullptr);
    std::uint64_t rip = 0, rsp = 0, rbp = 0;
    int slot = -1; /* where the faulting address was found in the context */
#ifndef SLOPFIN_HOST
    if (context != nullptr)
    {
        const auto &machine = static_cast<const ucontext_t *>(context)->uc_mcontext;
        rip = static_cast<std::uint64_t>(machine.mc_rip);
        rsp = static_cast<std::uint64_t>(machine.mc_rsp);
        rbp = static_cast<std::uint64_t>(machine.mc_rbp);
        /* FW 8.20 signal-context layout differs from the SDK mcontext_t. Extract validated
         * registers from the actual block. */
        const auto *words = reinterpret_cast<const std::uint64_t *>(context);
        /* A null instruction pointer matches empty context words; validate the frame before
         * accepting an address-zero match. */
        const auto plausible = [](std::uint64_t candidate_rip, std::uint64_t candidate_rsp)
        {
            return candidate_rsp > 0x100000u && candidate_rsp < (1ull << 48) &&
                   candidate_rip < (1ull << 48);
        };
        if (!plausible(rip, rsp))
            for (int at = 8; at < 96; ++at)
                if (words[at] == address && plausible(words[at + 3], words[at + 6]))
                {
                    rip = words[at + 3];
                    rsp = words[at + 6];
                    rbp = words[at - 8];
                    slot = at;
                    break;
                }
    }
#else
    /* The preview runs the same walk, which is how it is tested at all. */
    if (context != nullptr)
    {
        const auto &machine = static_cast<const ucontext_t *>(context)->uc_mcontext;
        rip = static_cast<std::uint64_t>(machine.gregs[REG_RIP]);
        rsp = static_cast<std::uint64_t>(machine.gregs[REG_RSP]);
        rbp = static_cast<std::uint64_t>(machine.gregs[REG_RBP]);
    }
#endif
    const auto emit = [&](Report &out)
    {
        out.text("=== SlopFin crash ===").line();
        out.text("signal  ").text(signal_name(number)).text("  (").number(number).text(")").line();
        out.text("address 0x").hex(address, 12).line();
        if (rip != 0)
        {
            out.text("rip     0x").hex(rip, 12).line();
            out.text("rsp     0x").hex(rsp, 12).line();
            out.text("rbp     0x").hex(rbp, 12).line();
            if (slot >= 0)
                out.text("context mc_addr at word ").number(slot).line();
        }
        write_state(out);
        /* Even with no frame chain the stack is worth printing: the return
           address into SlopFin is in there. */
        if (rsp > 0x100000u)
            write_backtrace(out, rbp, rsp);
        write_trace_tail(out, 40);
        out.text("=== end ===").line().line();
    };
    {
        Report newest(kCrash, false);
        if (newest.good())
            emit(newest);
    }
    {
        Report history(kHistory, true);
        if (history.good())
            emit(history);
    }
}

void fatal_handler(int number, siginfo_t *info, void *context) noexcept
{
    /* A second fault inside the handler must not spin: report once, then go. */
    if (g_reported.exchange(true, std::memory_order_acq_rel))
        _exit(1);
    write_fault(number, info, context);
    (void)unlink(kSession);
    _exit(1);
}

/*
 * ShellCore closes an application with an ordinary termination signal when the
 * user chooses Close Game. That is not a crash. Remove the heartbeat file
 * before leaving so the next launch does not turn a deliberate close into an
 * "unclean exit" report. A hard kill (SIGKILL, GPU/process death, etc.) cannot
 * run this handler, so those still leave the session behind for diagnosis.
 */
void clean_exit_handler(int) noexcept
{
    (void)unlink(kSession);
    _exit(0);
}

void terminate_handler() noexcept
{
    if (!g_reported.exchange(true, std::memory_order_acq_rel))
    {
        Report out(kCrash, false);
        if (out.good())
        {
            out.text("=== SlopFin terminate ===").line();
            out.text("an exception left the program with nowhere to go").line();
            write_state(out);
            write_trace_tail(out, 40);
            out.text("=== end ===").line().line();
        }
        Report history(kHistory, true);
        if (history.good())
        {
            history.text("=== SlopFin terminate ===").line();
            write_state(history);
            history.text("=== end ===").line().line();
        }
    }
    (void)unlink(kSession);
    _exit(1);
}

void start_heartbeat(std::uint64_t cores) noexcept;

/* A worker heartbeat detects abrupt process removal that cannot run the signal handler. */
void *heartbeat(void *) noexcept
{
    /*
     * One wake a second, and a write only when something actually changed or
     * every fifteen seconds regardless. The thread costs a wake-up and a
     * kilobyte; it never runs on the render core (the affinity is set by the
     * caller), and it never writes while nothing is happening.
     *
     */
    for (unsigned tick = 0;; ++tick)
    {
        (void)sceKernelUsleep(1000u * 1000u);
        if (g_dirty.exchange(false, std::memory_order_acq_rel) || (tick % 15) == 0)
            write_session_file();
    }
    return nullptr;
}

void start_heartbeat(std::uint64_t cores) noexcept
{
    if (g_beating.exchange(true, std::memory_order_acq_rel))
        return;
    void *thread = nullptr;
    pthread_attr_t attributes;
    const bool have = pthread_attr_init(&attributes) == 0;
    if (have)
        (void)pthread_attr_setstacksize(&attributes, 128u * 1024u);
    const int created = scePthreadCreate(&thread, have ? &attributes : nullptr, heartbeat, nullptr,
                                         "slopfin-session");
    if (have)
        (void)pthread_attr_destroy(&attributes);
    if (created != 0)
        g_beating.store(false, std::memory_order_release);
    else if (cores != 0)
        (void)scePthreadSetaffinity(thread, cores);
}

/* A session file left behind is a run that died without saying so. */
void keep_unfinished_session() noexcept
{
    const int fd = open(kSession, kReadOnly);
    if (fd < 0)
        return;
    static char body[4096];
    std::size_t used = 0;
    for (;;)
    {
        const long got = read(fd, body + used, sizeof(body) - used - 1);
        if (got <= 0)
            break;
        used += static_cast<std::size_t>(got);
        if (used + 1 >= sizeof(body))
            break;
    }
    (void)close(fd);
    (void)unlink(kSession);
    if (used == 0)
        return;
    {
        Report out(kLastRun, false);
        if (out.good())
        {
            out.text("the run before this one ended without shutting down").line();
            out.text(std::string_view{body, used});
        }
    }
    {
        Report history(kHistory, true);
        if (history.good())
        {
            history.text("=== SlopFin unclean exit (found at next start) ===").line();
            history.text(std::string_view{body, used});
            history.text("=== end ===").line().line();
        }
    }
    trace::mark(
        "crash: the previous run ended without shutting down; see /data/slopfin-lastrun.txt");
}
} // namespace

void install() noexcept
{
    if (g_installed)
        return;
    g_installed = true;
    keep_unfinished_session();
    write_session_file();

    std::set_terminate(terminate_handler);

    /*
     * The handler needs somewhere to run. A thread that faults by running out
     * of stack has none left, and the handler then faults too -- the process
     * dies with nothing written, which is exactly the case worth catching.
     */
    static std::array<char, 128 * 1024> guard_stack;
    stack_t alternate{};
    alternate.ss_sp = guard_stack.data();
    alternate.ss_size = guard_stack.size();
    alternate.ss_flags = 0;
    const bool on_its_own_stack = sigaltstack(&alternate, nullptr) == 0;

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = fatal_handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER | (on_its_own_stack ? SA_ONSTACK : 0);
    for (int number : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS})
        (void)sigaction(number, &action, nullptr);

    struct sigaction clean_action;
    std::memset(&clean_action, 0, sizeof(clean_action));
    clean_action.sa_handler = clean_exit_handler;
    for (int number : {SIGTERM, SIGINT, SIGHUP})
        (void)sigaction(number, &clean_action, nullptr);

    trace::mark("crash: handlers installed");
}

void watch(std::uint64_t cores) noexcept
{
    start_heartbeat(cores);
}

void phase(std::string_view what) noexcept
{
    if (std::strncmp(g_phase, what.data(),
                     what.size() < kPhraseBytes - 1 ? what.size() : kPhraseBytes - 1) == 0 &&
        std::strlen(g_phase) == what.size())
        return;
    copy_phrase(g_phase, what);
    g_dirty.store(true, std::memory_order_release);
}

void detail(std::string_view what) noexcept
{
    copy_phrase(g_detail, what);
    g_dirty.store(true, std::memory_order_release);
}

void note(std::string_view what) noexcept
{
    const unsigned at = g_notes.written.fetch_add(1, std::memory_order_acq_rel);
    const std::size_t slot = at % kNotes;
    g_notes.frame[slot] = g_frames.load(std::memory_order_relaxed);
    copy_phrase(g_notes.text[slot], what);
}

void frame(int frames) noexcept
{
    g_frames.store(frames, std::memory_order_relaxed);
}

void finish() noexcept
{
    (void)unlink(kSession);
}

/* ---------------------------------------------------------------- sending */

namespace
{
/* A report is identified by its size and its first line, which is enough to
   tell a new crash from the one already sent without hashing anything. */
std::string fingerprint_of(const std::string &body) noexcept
{
    std::string first = body.substr(0, body.find('\n'));
    return std::to_string(body.size()) + "|" + first;
}

std::string read_file(const char *path, std::size_t limit) noexcept
{
    const int fd = open(path, kReadOnly);
    if (fd < 0)
        return {};
    std::string body;
    char chunk[2048];
    for (;;)
    {
        const long got = read(fd, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        body.append(chunk, static_cast<std::size_t>(got));
        if (body.size() >= limit)
            break;
    }
    (void)close(fd);
    return body;
}

std::string newest_report() noexcept
{
    if (std::string crash = read_file(kCrash, 64 * 1024); !crash.empty())
        return crash;
    return read_file(kLastRun, 64 * 1024);
}
} // namespace

bool report_available() noexcept
{
    return !newest_report().empty();
}

bool report_waiting() noexcept
{
    /* A leftover heartbeat proves only that the process disappeared. On PS5,
       ordinary Close Game can terminate us without delivering a signal, so an
       unfinished session is useful diagnostic evidence but is not, by itself,
       enough to accuse the previous run of crashing. Only a fatal handler's
       report interrupts startup. */
    const std::string body = read_file(kCrash, 64 * 1024);
    if (body.empty())
        return false;
    const std::string fingerprint = fingerprint_of(body);
    return read_file(kSentMark, 512) != fingerprint && read_file(kOfferedMark, 512) != fingerprint;
}

void report_offered() noexcept
{
    const std::string mark = fingerprint_of(read_file(kCrash, 64 * 1024));
    if (mark.empty())
        return;
    Report out(kOfferedMark, false);
    if (out.good())
        out.text(mark);
}

std::string report_kind() noexcept
{
    const std::string body = newest_report();
    if (body.find("SlopFin crash") != std::string::npos)
        return "crash";
    if (body.find("SlopFin terminate") != std::string::npos)
        return "terminate";
    return body.empty() ? "report" : "unclean-exit";
}

std::string report_summary() noexcept
{
    const std::string body = newest_report();
    if (body.empty())
        return "Nothing to send";
    /* The line that says what went wrong, or what the app was doing. */
    for (const char *wanted : {"signal  ", "doing   "})
    {
        const std::size_t at = body.find(wanted);
        if (at != std::string::npos)
        {
            const std::size_t end = body.find('\n', at);
            return body.substr(at, end == std::string::npos ? std::string::npos : end - at);
        }
    }
    return body.substr(0, body.find('\n'));
}

std::string report_body() noexcept
{
    std::string out = "SlopFin report\nbuild   " __DATE__ " " __TIME__ "\n\n";
    if (const std::string crash = read_file(kCrash, 64 * 1024); !crash.empty())
        out += "======== last crash ========\n" + crash + "\n";
    if (const std::string last = read_file(kLastRun, 16 * 1024); !last.empty())
        out += "======== the run before this one ========\n" + last + "\n";
    if (const std::string session = read_file(kSession, 8 * 1024); !session.empty())
        out += "======== this run ========\n" + session + "\n";
#ifdef SLOPFIN_HOST
    const std::string trace = read_file("slopfin-trace.txt", 128 * 1024);
#else
    const std::string trace = read_file("/data/slopfin-trace.txt", 128 * 1024);
#endif
    if (!trace.empty())
    {
        /* The start of the trace names the build and the hardware; the end is
           what was happening. Both are worth more than the middle. */
        out += "======== trace (start) ========\n" + trace.substr(0, 4096) + "\n";
        if (trace.size() > 8192)
            out += "======== trace (end) ========\n" + trace.substr(trace.size() - 8192) + "\n";
    }
    return out;
}

void report_sent() noexcept
{
    const std::string mark = fingerprint_of(newest_report());
    Report out(kSentMark, false);
    if (out.good())
        out.text(mark);
}

void report_forget() noexcept
{
    (void)unlink(kCrash);
    (void)unlink(kLastRun);
    (void)unlink(kSentMark);
    (void)unlink(kOfferedMark);
}

void self_test(int kind) noexcept
{
    note("deliberate fault, self test");
    switch (kind)
    {
    case 1:
        *reinterpret_cast<volatile int *>(0x10) = 1; /* SIGSEGV */
        break;
    case 2:
        std::terminate();
    default:
        raise(SIGABRT);
        break;
    }
}

} // namespace slopfin::crash
