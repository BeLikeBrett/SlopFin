/* Copyright (C) 2026 Brett
 * SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Installs a downloaded SlopFin package over the installed one, then opens it
 * again. Run by the ELF loader at the app's request; the app has already
 * verified the download, and this checks it again before anything changes.
 *
 * Sony's installer only takes a URL, so the package is served from this
 * process on 127.0.0.1 for as long as the install reads it, and nowhere else.
 */
#include "../../src/update/files.h"
#include "../../src/update/close.h"
#include "../../src/update/pkg_request.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <ps5/kernel.h>
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);
int sceSystemServiceLaunchApp(const char *, char **, void *);
int sceLncUtilGetAppId(const char *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppUnInstall(const char *);

/* AppInstUtil's request. Sony reads the eight bytes after icon_url: zero is
   a plain install, which replaces an installed copy; anything else is taken
   as a patch and refused. The rest stays zero for firmware that reads on. */
typedef struct
{
    const char *uri, *ex_uri, *playgo_scenario_id, *content_id, *content_name, *icon_url;
    uint64_t plain_install;
    uint64_t reserved[7];
} install_meta;
typedef struct
{
    char content_id[0x30];
    int content_type, content_platform;
} install_pkg_info;
typedef struct
{
    char languages[30][8];
    char scenario_ids[64][3];
    char content_ids[64][0x30];
    unsigned char unknown[6480];
} install_playgo;
int sceAppInstUtilInstallByPackage(install_meta *, install_pkg_info *, install_playgo *);

static const char *base = "/data/slopfin/update";
static const char *package_path = "/data/slopfin/update/release.pkg";

static void result(const char *message)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/result", base);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd >= 0)
    {
        (void)write(fd, message, strlen(message));
        (void)fsync(fd);
        close(fd);
    }
}

static int become_shellcore(void)
{
    const pid_t self = getpid();
    static const uint8_t caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t attrs[32] = {0x80};
    if (kernel_set_ucred_uid(self, 0) || kernel_set_ucred_ruid(self, 0) ||
        kernel_set_ucred_svuid(self, 0) || kernel_set_ucred_caps(self, caps) ||
        kernel_set_ucred_attrs(self, attrs))
        return -1;
    return kernel_set_ucred_authid(self, 0x4801000000000013UL);
}

static void pause_ms(long ms)
{
    struct timespec wait = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&wait, NULL);
}

/* ------------------------------------------------------------- serving */

static int g_package = -1;
static uint64_t g_size = 0;
static volatile int g_stop = 0;
static volatile uint64_t g_served = 0; /* furthest byte any response reached */

static int send_all(int fd, const void *data, size_t size)
{
    const char *at = data;
    while (size)
    {
        const ssize_t sent = send(fd, at, size, 0);
        if (sent <= 0)
            return -1;
        at += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int send_range(int fd, uint64_t start, uint64_t end)
{
    static __thread char buffer[65536];
    for (uint64_t at = start; at <= end;)
    {
        const size_t want = (size_t)(end - at + 1 < sizeof(buffer) ? end - at + 1 : sizeof(buffer));
        const ssize_t got = pread(g_package, buffer, want, (off_t)at);
        if (got <= 0 || send_all(fd, buffer, (size_t)got) < 0)
            return -1;
        at += (uint64_t)got;
        if (at > g_served)
            g_served = at;
    }
    return 0;
}

/* The SDK's libc has no memmem. */
static char *find_headers_end(char *text, size_t length)
{
    for (size_t i = 0; i + 4 <= length; ++i)
        if (!memcmp(text + i, "\r\n\r\n", 4))
            return text + i;
    return NULL;
}

/* One connection, as many requests as the installer sends on it. */
static void *serve(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    const struct timeval timeout = {60, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char request[8192];
    size_t have = 0;
    while (!g_stop)
    {
        char *headers_end = NULL;
        while (!(headers_end = find_headers_end(request, have)))
        {
            if (have == sizeof(request))
                goto done;
            const ssize_t got = recv(fd, request + have, sizeof(request) - have, 0);
            if (got <= 0)
                goto done;
            have += (size_t)got;
        }
        *headers_end = 0;
        const int head = !strncmp(request, "HEAD ", 5);
        const int get = !strncmp(request, "GET ", 4);
        const char *path = request + (head ? 5 : 4);
        const int ours = (head || get) && !strncmp(path, "/slopfin.pkg ", 13);
        const int close_after = strcasestr(request, "\r\nConnection: close") != NULL;
        uint64_t start = 0, end = g_size - 1;
        int partial = 0;
        const char *range = strcasestr(request, "\r\nRange: bytes=");
        if (range)
        {
            unsigned long long a = 0, b = 0;
            const char *spec = range + 15;
            if (sscanf(spec, "%llu-%llu", &a, &b) == 2)
                start = a, end = b < g_size ? b : g_size - 1, partial = 1;
            else if (sscanf(spec, "%llu-", &a) == 1)
                start = a, partial = 1;
        }
        char header[512];
        int length;
        if (!ours)
            length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        else if (start > end || start >= g_size)
            length =
                snprintf(header, sizeof(header),
                         "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\n"
                         "Content-Length: 0\r\nConnection: %s\r\n\r\n",
                         (unsigned long long)g_size, close_after ? "close" : "keep-alive");
        else if (partial)
            length = snprintf(
                header, sizeof(header),
                "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
                "Accept-Ranges: bytes\r\nContent-Length: %llu\r\n"
                "Content-Range: bytes %llu-%llu/%llu\r\nConnection: %s\r\n\r\n",
                (unsigned long long)(end - start + 1), (unsigned long long)start,
                (unsigned long long)end, (unsigned long long)g_size,
                close_after ? "close" : "keep-alive");
        else
            length =
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                         "Accept-Ranges: bytes\r\nContent-Length: %llu\r\nConnection: %s\r\n\r\n",
                         (unsigned long long)g_size, close_after ? "close" : "keep-alive");
        if (send_all(fd, header, (size_t)length) < 0)
            break;
        if (ours && get && start <= end && start < g_size && send_range(fd, start, end) < 0)
            break;
        if (!ours || close_after)
            break;
        const size_t used = (size_t)(headers_end + 4 - request);
        memmove(request, request + used, have - used);
        have -= used;
    }
done:
    close(fd);
    return NULL;
}

static void *listener(void *arg)
{
    const int listen_fd = (int)(intptr_t)arg;
    while (!g_stop)
    {
        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        const int fd = accept(listen_fd, (struct sockaddr *)&peer, &length);
        if (fd < 0)
            continue;
        if (peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
        {
            close(fd);
            continue;
        }
        pthread_t thread;
        if (pthread_create(&thread, NULL, serve, (void *)(intptr_t)fd) == 0)
            pthread_detach(thread);
        else
            close(fd);
    }
    return NULL;
}

/* Listens on an ephemeral port of 127.0.0.1 only; returns the port or -1. */
static int start_server(int *listen_out)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    pthread_t thread;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 8) ||
        getsockname(fd, (struct sockaddr *)&address, &length) ||
        pthread_create(&thread, NULL, listener, (void *)(intptr_t)fd))
    {
        close(fd);
        return -1;
    }
    pthread_detach(thread);
    *listen_out = fd;
    return ntohs(address.sin_port);
}

/* ------------------------------------------------------------ checking */

/* The installed copy's version, from its metadata. */
static int installed_version(const char *title, char out[11])
{
    char path[128];
    snprintf(path, sizeof(path), "/user/appmeta/%s/param.json", title);
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    char text[16384];
    const ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0)
        return -1;
    text[got] = 0;
    const char *key = strstr(text, "\"contentVersion\"");
    const char *quote = key ? strchr(key + 16, '"') : NULL;
    if (!quote || strlen(quote) < 12 || quote[11] != '"')
        return -1;
    memcpy(out, quote + 1, 10);
    out[10] = 0;
    return sf_pkg_version_valid(out) ? 0 : -1;
}

/* The installed image is the package without its trailing install metadata. */
static int installed_matches(const char *title)
{
    char path[128];
    snprintf(path, sizeof(path), "/user/app/%s/app.pkg", title);
    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 0x10000 || (uint64_t)st.st_size > g_size)
    {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    static unsigned char a[65536], b[65536];
    int same = 1;
    for (off_t at = 0; same && at < st.st_size; at += (off_t)sizeof(a))
    {
        const size_t want =
            (size_t)(st.st_size - at < (off_t)sizeof(a) ? st.st_size - at : (off_t)sizeof(a));
        same = pread(fd, a, want, at) == (ssize_t)want &&
               pread(g_package, b, want, at) == (ssize_t)want && !memcmp(a, b, want);
    }
    close(fd);
    return same;
}

static int content_is(const char *title)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "UP9000-%s_00-", title);
    unsigned char cnt[0x80];
    for (uint64_t at = 0x10000; at + sizeof(cnt) <= g_size; at += 0x10000)
        if (pread(g_package, cnt, sizeof(cnt), (off_t)at) == (ssize_t)sizeof(cnt) && !memcmp(cnt,
                                                                                             "\x7f"
                                                                                             "CNT",
                                                                                             4))
            return sf_pkg_content_matches(cnt, sizeof(cnt), prefix) == 0;
    return 0;
}

static void relaunch(const char *title)
{
    unsigned char options[1024] = {0};
    (void)sceSystemServiceLaunchApp(title, NULL, options);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char lock[256];
    snprintf(lock, sizeof(lock), "%s/install.lock", base);
    int lock_fd = open(lock, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (lock_fd < 0)
    {
        result(errno == EEXIST ? "An update is already running or was interrupted. Restart the "
                                 "PS5, then try again."
                               : "Could not create the installer lock. Your app was not changed.");
        return 1;
    }
    close(lock_fd);
#define FAIL(message)                                                                              \
    do                                                                                             \
    {                                                                                              \
        result(message);                                                                           \
        unlink(lock);                                                                              \
        return 1;                                                                                  \
    } while (0)
    if (become_shellcore() < 0)
        FAIL("Could not start the update helper. Your app was not changed.");
    const intptr_t root = kernel_get_root_vnode();
    if (kernel_set_proc_rootdir(getpid(), root) || kernel_set_proc_jaildir(getpid(), root))
        FAIL("Could not reach the update folder. Your app was not changed.");

    char text[SF_PKG_REQUEST_MAX];
    sf_pkg_request request;
    const int request_fd = open("/data/slopfin/update/pkg-request", O_RDONLY | O_NOFOLLOW);
    const ssize_t got = request_fd < 0 ? -1 : read(request_fd, text, sizeof(text));
    if (request_fd >= 0)
        close(request_fd);
    if (got <= 0 || sf_pkg_request_parse(text, (size_t)got, &request))
        FAIL("The update request was not valid. Your app was not changed.");
    if (kill((pid_t)request.pid, 0) < 0 || sceLncUtilGetAppId(request.title) <= 0)
        FAIL("The app asking for this update is not running. Your app was not changed.");

    char hex[65];
    g_package = open(package_path, O_RDONLY | O_NOFOLLOW);
    struct stat st;
    if (g_package < 0 || fstat(g_package, &st) || (uint64_t)st.st_size != request.size ||
        sf_update_hash_fd(g_package, hex) || strcmp(hex, request.sha256))
        FAIL("The downloaded package did not verify. Download the update again. Your app was not "
             "changed.");
    g_size = request.size;
    if (!content_is(request.title))
        FAIL("The downloaded package is for a different app. Your app was not changed.");

    sceUserServiceInitialize(NULL);
    unlink("/data/slopfin/update/closed");
    result("ready");
    /* The app releases its display and decoder, then exits. */
    int stopped = 0;
    int close_requested = 0;
    for (unsigned i = 0; i < 60 && !stopped; ++i)
    {
        if (!close_requested &&
            sf_update_close_stopped(request.pid, request.title, "/data/slopfin/update/closed") >= 0)
            close_requested = 1;
        stopped = kill((pid_t)request.pid, 0) < 0 && errno == ESRCH &&
                  sceLncUtilGetAppId(request.title) <= 0;
        if (!stopped)
            pause_ms(500);
    }
    if (!stopped)
    {
        sceUserServiceTerminate();
        FAIL("The app did not close. Nothing was installed. Try again.");
    }

    int listen_fd = -1;
    const int port = start_server(&listen_fd);
    if (port < 0 || sceAppInstUtilInitialize() < 0)
    {
        relaunch(request.title);
        sceUserServiceTerminate();
        FAIL("Could not start the system installer. Your app was not changed.");
    }
    /*
     * Never install over the copy that just closed. Replacing an installed
     * package straight away, while its mount was still being torn down, took
     * a test console offline. Instead, as the console tests do: let the old
     * package unmount, uninstall it, see it gone, then install fresh. The
     * app's settings live in /data/slopfin, outside the package.
     */
    char installed_dir[64];
    snprintf(installed_dir, sizeof(installed_dir), "/user/app/%s", request.title);
    pause_ms(5000);
    const int removed = sceAppInstUtilAppUnInstall(request.title);
    int gone = 0;
    for (unsigned i = 0; i < 120 && !gone; ++i)
    {
        struct stat dir;
        gone = stat(installed_dir, &dir) < 0 && errno == ENOENT;
        if (!gone)
            pause_ms(500);
    }
    if (removed < 0)
    {
        g_stop = 1;
        relaunch(request.title);
        sceUserServiceTerminate();
        FAIL("Could not remove the installed version for the update. Your app was not changed.");
    }
    /* From here on the old copy is going or gone; the download is kept so it
       can be installed by hand, and no message may say nothing changed. */
#define REMOVED_HINT                                                                               \
    " Install /data/slopfin/update/release.pkg with your package installer; your settings are "    \
    "kept."
    if (!gone)
    {
        g_stop = 1;
        sceUserServiceTerminate();
        FAIL("The installed version did not finish uninstalling." REMOVED_HINT);
    }
    pause_ms(3000);
    char uri[64];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%d/slopfin.pkg", port);
    install_meta meta;
    install_pkg_info info;
    static install_playgo playgo;
    memset(&meta, 0, sizeof(meta));
    memset(&info, 0, sizeof(info));
    memset(&playgo, 0, sizeof(playgo));
    meta.uri = uri;
    meta.ex_uri = meta.playgo_scenario_id = meta.content_id = meta.icon_url = "";
    meta.content_name = "SlopFin";
    const int accepted = sceAppInstUtilInstallByPackage(&meta, &info, &playgo);
    if (accepted < 0)
    {
        g_stop = 1;
        sceUserServiceTerminate();
        char message[256];
        snprintf(message, sizeof(message),
                 "The system installer refused the update (0x%08x)." REMOVED_HINT,
                 (unsigned)accepted);
        FAIL(message);
    }

    /* Finished when the installed copy is the new version, matches the
       package byte for byte and has stopped growing. Ten minutes at most. */
    int installed = 0;
    for (unsigned i = 0; i < 600 && !installed; ++i)
    {
        pause_ms(1000);
        char version[11];
        installed = g_served >= g_size - 0x10000 &&
                    installed_version(request.title, version) == 0 &&
                    !strcmp(version, request.version) && installed_matches(request.title);
    }
    g_stop = 1;
    close(listen_fd);
    if (installed)
    {
        unlink(package_path);
        unlink("/data/slopfin/update/pkg-request");
        result("Update installed successfully.");
    }
    else
        result("The update did not finish installing." REMOVED_HINT);
    unlink(lock);
    relaunch(request.title);
    sceUserServiceTerminate();
    return installed ? 0 : 1;
}
