// HD60 S as an OBS source, video and audio in one.
//
// The plugin does no USB work. It listens on a unix socket and the `demux`
// process connects to it and sends whole frames and blocks of PCM, already
// separated out of the card's stream. That keeps the driver a plain command
// line program that can be run and debugged on its own, and keeps OBS out of
// the isochronous path.
//
// Adding the source starts the capture: the plugin spawns hd60s-obs in its own
// process group and kills the group again when the source goes away. The path
// is baked in at build time and can be changed in the source's properties.
// Turning that off leaves the socket listening for a hd60s-obs run by hand.
//
// Timestamps arrive relative to the start of the connection. We add our own
// os_gettime_ns() base so the two processes never have to agree on a clock.

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <media-io/video-io.h>

#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../src/hd60s_ipc.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("hd60s", "en-US")

struct hd60s {
    obs_source_t *source;
    char *sockpath;

    int listen_fd;
    volatile int conn_fd;

    pthread_t thread;
    bool thread_ok;
    volatile bool quit;

    uint64_t base;          // os_gettime_ns() at the first packet of a connection
    bool have_base;

    struct obs_source_frame frame;
    uint8_t *vbuf;

    volatile int colour;    // COLOUR_*, from the source's properties
    int colour_applied;     // what frame.color_matrix currently holds

    pid_t child;
};

// What the HDMI source sends. The card's colour stage is set up by the cold
// init, which was captured from a YCbCr source, and it never looks at the AVI
// InfoFrame. An RGB source (a Nintendo Switch, most consoles, some PCs) goes
// through the same stage unconverted: green lands in the luma slot, blue in Cb
// and red in Cr, each with its own gain and offset. On screen that is a green
// picture where dark greys should be and pink where white should be.
enum { COLOUR_YCBCR = 0, COLOUR_RGB = 1 };

// Undo that for an RGB source. Rows are output R, G, B; columns are the Y, Cb,
// Cr inputs (0..1) and an offset, which is how OBS's YUV_to_RGB reads them.
// Each channel is mapped back with a straight line fitted to two neutral
// points read off a Switch home screen captured through this driver: the dark
// theme background (45) and white text (255). Fitted from one screenshot, so
// treat it as close rather than exact; the right fix is programming the card's
// own matrix from the InfoFrame, which needs an RGB source on the bench.
static const float kRgbMatrix[16] = {
    0.000000f, 0.000000f, 5.000000f, -1.960784f,  // R from Cr: 109..151 -> 45..255
    1.909091f, 0.000000f, 0.000000f, -0.377540f,  // G from Y:   74..184 -> 45..255
    0.000000f, 4.468085f, 0.000000f, -1.663330f,  // B from Cb: 105..152 -> 45..255
    0.000000f, 0.000000f, 0.000000f,  1.000000f,
};

static void apply_colour(struct hd60s *h, int colour)
{
    if (colour == COLOUR_RGB) {
        memcpy(h->frame.color_matrix, kRgbMatrix, sizeof kRgbMatrix);
        for (int i = 0; i < 3; i++) {
            h->frame.color_range_min[i] = 0.0f;
            h->frame.color_range_max[i] = 1.0f;
        }
        h->frame.full_range = true;
    } else {
        // The card is BT.709 limited range; without this the picture comes out
        // washed out and slightly green.
        video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL,
                                    h->frame.color_matrix,
                                    h->frame.color_range_min,
                                    h->frame.color_range_max);
        h->frame.full_range = false;
    }
    h->colour_applied = colour;
}

#ifndef HD60S_CAPTURE_CMD
#define HD60S_CAPTURE_CMD "hd60s-obs"
#endif
#define HD60S_CHILD_LOG "/tmp/hd60s-capture.log"

static const char *hd60s_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Elgato HD60 S";
}

// Read exactly n bytes, or fail. A short read on a stream socket is normal.
static bool read_all(int fd, void *p, size_t n, volatile bool *quit)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = recv(fd, b, n, 0);
        if (r > 0) { b += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR && !*quit) continue;
        return false;
    }
    return true;
}

static void push_video(struct hd60s *h, const uint8_t *data, uint64_t ts)
{
    // Changed in properties on the UI thread; picked up here so the matrix is
    // never rewritten while a frame is being handed to OBS.
    int colour = h->colour;
    if (colour != h->colour_applied) apply_colour(h, colour);

    h->frame.data[0] = (uint8_t *)data;
    h->frame.linesize[0] = HD60S_W * 2;
    h->frame.width = HD60S_W;
    h->frame.height = HD60S_H;
    h->frame.format = VIDEO_FORMAT_YUY2;
    h->frame.timestamp = h->base + ts;
    obs_source_output_video(h->source, &h->frame);
}

static void push_audio(struct hd60s *h, const uint8_t *data, uint32_t len, uint64_t ts)
{
    struct obs_source_audio a = {0};
    a.data[0] = data;
    a.frames = len / 4;                 // 16 bit stereo
    a.speakers = SPEAKERS_STEREO;
    a.format = AUDIO_FORMAT_16BIT;
    a.samples_per_sec = HD60S_RATE;
    a.timestamp = h->base + ts;
    obs_source_output_audio(h->source, &a);
}

static void serve(struct hd60s *h, int fd)
{
    uint8_t *abuf = bmalloc(65536);
    h->have_base = false;

    while (!h->quit) {
        struct hd60s_hdr hdr;
        if (!read_all(fd, &hdr, sizeof hdr, &h->quit)) break;
        if (hdr.magic != HD60S_MAGIC) {
            blog(LOG_ERROR, "[hd60s] bad magic 0x%08x, dropping the connection", hdr.magic);
            break;
        }
        if (!h->have_base) {
            // The first packet defines the zero point. Everything after it is
            // offset from the same instant, so video and audio stay locked.
            h->base = os_gettime_ns();
            h->have_base = true;
        }
        if (hdr.type == HD60S_VIDEO) {
            if (hdr.len != HD60S_W * 2 * HD60S_H) {
                blog(LOG_ERROR, "[hd60s] frame is %u bytes, expected %u",
                     hdr.len, HD60S_W * 2 * HD60S_H);
                break;
            }
            if (!read_all(fd, h->vbuf, hdr.len, &h->quit)) break;
            push_video(h, h->vbuf, hdr.ts);
        } else if (hdr.type == HD60S_AUDIO) {
            if (hdr.len > 65536) break;
            if (!read_all(fd, abuf, hdr.len, &h->quit)) break;
            push_audio(h, abuf, hdr.len, hdr.ts);
        } else {
            blog(LOG_ERROR, "[hd60s] unknown packet type %u", hdr.type);
            break;
        }
    }
    bfree(abuf);
}

static void *hd60s_thread(void *param)
{
    struct hd60s *h = param;
    os_set_thread_name("hd60s");

    while (!h->quit) {
        int fd = accept(h->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;                       // the socket was closed under us
        }
        blog(LOG_INFO, "[hd60s] capture connected");
        h->conn_fd = fd;
        serve(h, fd);
        h->conn_fd = -1;
        close(fd);
        blog(LOG_INFO, "[hd60s] capture disconnected");
        obs_source_output_video(h->source, NULL);
    }
    return NULL;
}

// A second copy of the source would otherwise unlink the first one's socket,
// take the name, and start a second capture that cannot open the USB device
// because the first still holds it. The result is that both go black. If
// something answers on the path, leave it alone.
static bool socket_taken(const char *path)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    bool live = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
    close(fd);
    return live;
}

static bool start_listening(struct hd60s *h)
{
    struct sockaddr_un sa;

    if (socket_taken(h->sockpath)) {
        blog(LOG_WARNING, "[hd60s] another source already owns %s. "
                          "This one stays blank; delete it, or give it its own "
                          "socket path in properties.", h->sockpath);
        return false;
    }

    h->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (h->listen_fd < 0) {
        blog(LOG_ERROR, "[hd60s] socket: %s", strerror(errno));
        return false;
    }
    // A socket left behind by a crash would make bind fail for ever.
    unlink(h->sockpath);
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", h->sockpath);
    if (bind(h->listen_fd, (struct sockaddr *)&sa, sizeof sa) ||
        listen(h->listen_fd, 1)) {
        blog(LOG_ERROR, "[hd60s] cannot listen on %s: %s", h->sockpath, strerror(errno));
        close(h->listen_fd);
        h->listen_fd = -1;
        return false;
    }
    chmod(h->sockpath, 0600);
    blog(LOG_INFO, "[hd60s] listening on %s", h->sockpath);
    return true;
}

// hd60s-obs is a shell script that starts two more processes, so it goes into
// its own process group and the whole group is signalled. Its output lands in
// /tmp/hd60s-capture.log, which is the first place to look when the picture
// stays black.
static void spawn_capture(struct hd60s *h, const char *cmd)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    char *argv[] = { (char *)cmd, (char *)"--init", NULL };
    extern char **environ;

    // The child has to connect to the same socket this source is listening on,
    // and hd60s-obs takes that from the environment.
    struct dstr sockenv = {0};
    dstr_printf(&sockenv, "HD60S_SOCK=%s", h->sockpath);
    size_t n = 0;
    while (environ[n]) n++;
    char **env = bmalloc(sizeof(char *) * (n + 2));
    memcpy(env, environ, sizeof(char *) * n);
    env[n] = sockenv.array;
    env[n + 1] = NULL;

    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, HD60S_CHILD_LOG,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    int rc = posix_spawn(&h->child, cmd, &fa, &attr, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    bfree(env);
    dstr_free(&sockenv);

    if (rc) {
        h->child = 0;
        blog(LOG_ERROR, "[hd60s] cannot start %s: %s", cmd, strerror(rc));
    } else {
        blog(LOG_INFO, "[hd60s] started %s (pid %d), output in %s",
             cmd, (int)h->child, HD60S_CHILD_LOG);
    }
}

static void stop_capture(struct hd60s *h)
{
    if (!h->child) return;
    kill(-h->child, SIGINT);
    for (int i = 0; i < 100; i++) {
        if (waitpid(h->child, NULL, WNOHANG) == h->child) { h->child = 0; return; }
        os_sleep_ms(100);
    }
    kill(-h->child, SIGKILL);
    waitpid(h->child, NULL, 0);
    h->child = 0;
}

static void hd60s_destroy(void *data)
{
    struct hd60s *h = data;
    if (!h) return;

    stop_capture(h);
    h->quit = true;
    if (h->listen_fd >= 0) { shutdown(h->listen_fd, SHUT_RDWR); close(h->listen_fd); }
    if (h->conn_fd >= 0) shutdown(h->conn_fd, SHUT_RDWR);
    if (h->thread_ok) pthread_join(h->thread, NULL);
    if (h->listen_fd >= 0) unlink(h->sockpath);

    bfree(h->vbuf);
    bfree(h->sockpath);
    bfree(h);
}

static void *hd60s_create(obs_data_t *settings, obs_source_t *source)
{
    struct hd60s *h = bzalloc(sizeof *h);
    h->source = source;
    h->listen_fd = -1;
    h->conn_fd = -1;
    h->sockpath = bstrdup(obs_data_get_string(settings, "socket"));
    h->vbuf = bmalloc(HD60S_W * 2 * HD60S_H);

    h->colour = (int)obs_data_get_int(settings, "colour");
    apply_colour(h, h->colour);

    // A source that cannot listen still exists, it just carries no picture.
    // Destroying it here would leave OBS with a dead scene item and no clue why.
    if (start_listening(h)) {
        h->thread_ok = pthread_create(&h->thread, NULL, hd60s_thread, h) == 0;
        if (h->thread_ok && obs_data_get_bool(settings, "autostart"))
            spawn_capture(h, obs_data_get_string(settings, "command"));
    }
    return h;
}

static void hd60s_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "socket", HD60S_SOCK_DEFAULT);
    obs_data_set_default_string(settings, "command", HD60S_CAPTURE_CMD);
    obs_data_set_default_bool(settings, "autostart", true);
    obs_data_set_default_int(settings, "colour", COLOUR_YCBCR);
}

static void hd60s_update(void *data, obs_data_t *settings)
{
    struct hd60s *h = data;
    h->colour = (int)obs_data_get_int(settings, "colour");
}

static obs_properties_t *hd60s_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *p = obs_properties_create();
    obs_property_t *c = obs_properties_add_list(p, "colour", "Input colour",
                                                OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(c, "YCbCr (Mac, players)", COLOUR_YCBCR);
    obs_property_list_add_int(c, "RGB (Switch)", COLOUR_RGB);
    obs_property_set_long_description(c,
        "If the picture is green where it should be dark grey and pink where it "
        "should be white, the source is sending RGB. Pick RGB.");
    obs_properties_add_bool(p, "autostart", "Start the capture with this source");
    obs_properties_add_text(p, "command", "Capture command", OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, "socket", "Socket path", OBS_TEXT_DEFAULT);
    obs_property_set_long_description(
        obs_properties_get(p, "socket"),
        "Where the capture process connects. Changing it takes effect when the "
        "source is recreated.");
    return p;
}

static struct obs_source_info hd60s_source_info = {
    .id = "hd60s_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = hd60s_name,
    .create = hd60s_create,
    .destroy = hd60s_destroy,
    .get_defaults = hd60s_defaults,
    .update = hd60s_update,
    .get_properties = hd60s_properties,
    .icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void)
{
    obs_register_source(&hd60s_source_info);
    blog(LOG_INFO, "[hd60s] registered as \"%s\", flags 0x%x",
         obs_source_get_display_name(hd60s_source_info.id),
         obs_get_source_output_flags(hd60s_source_info.id));
    return true;
}
