// split an HD60 S capture into raw video and raw audio.
//
// The card sends one long byte stream on endpoint 0x83. Each video line is
// 3840 bytes of YUY2 followed by a short trailer:
//
//     ff 00  [ ff 02 <8 bytes audio> ff 00 ]  00 tt
//
// tt is 0x80 on an active line and 0xab on a vertical blanking line, which is
// what gives us the frame boundary: 1080 active lines, then 45 blanking ones.
// The optional middle block appears on roughly one line in three and carries
// two stereo samples, four little-endian int16 in L R L R order. 400 blocks per
// frame at 60 fps is 48000 Hz, which is where the sample rate comes from.
//
// Reads the raw stream on stdin. Writes 1920x1080 YUY2 frames and 48 kHz stereo
// s16le either to files or fifos:
//
//     demux --video /tmp/v.yuv --audio /tmp/a.pcm
//
// or to the OBS plugin over a unix socket, which is the normal way to run it:
//
//     demux --obs

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include "hd60s_ipc.h"

#define VID_BYTES (HD60S_W * 2)
#define ACTIVE    HD60S_H
#define FRAME_SZ  (VID_BYTES * ACTIVE)

// 10 ms of audio per packet. One packet per 8-byte block would be 24000 packets
// a second, which is all header and no payload.
#define ACHUNK (HD60S_RATE / 100 * 4)

static FILE *fv, *fa;
static int   sock = -1;
static unsigned char frame[FRAME_SZ];
static unsigned char line[VID_BYTES];
static unsigned char abuf[ACHUNK];
static size_t agot;

static unsigned long long nLine, nBlank, nFrame, nAud, nResync, nShort, nDrop;
static unsigned long long frameIdx, sampleIdx;
static volatile sig_atomic_t stop;
static void onint(int s) { (void)s; stop = 1; }

// ------------------------------------------------------------------ output

// The consumer is a fifo of 64 KB, a sixtieth of a frame, or a socket into
// another process. Either can stall, and a stall on the writing side would stop
// the parser, and with it the audio and the USB reader feeding us. So output
// runs on its own thread behind a short queue, and a full queue drops the
// oldest video frame rather than waiting.
struct pkt { unsigned char *data; uint32_t len; uint64_t ts; uint8_t type; };

#define QN 12
static struct pkt q[QN];
static int qhead, qcount, qdone;
static pthread_mutex_t qm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  qc = PTHREAD_COND_INITIALIZER;

static uint64_t nowNs(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static int writeAll(int fd, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

static void *outThread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&qm);
        while (!qcount && !qdone) pthread_cond_wait(&qc, &qm);
        if (!qcount && qdone) { pthread_mutex_unlock(&qm); return NULL; }
        struct pkt p = q[qhead];
        qhead = (qhead + 1) % QN; qcount--;
        pthread_mutex_unlock(&qm);

        if (sock >= 0) {
            struct hd60s_hdr h = { HD60S_MAGIC, p.type, {0,0,0}, p.len, p.ts };
            if (writeAll(sock, &h, sizeof h) || writeAll(sock, p.data, p.len)) {
                fprintf(stderr, "demux: OBS plugin went away\n");
                close(sock); sock = -1; stop = 1;
            }
        } else {
            FILE *f = p.type == HD60S_VIDEO ? fv : fa;
            if (f) fwrite(p.data, 1, p.len, f);
        }
        free(p.data);
    }
}

static void queue(uint8_t type, const unsigned char *src, uint32_t len, uint64_t ts) {
    unsigned char *b = malloc(len);
    if (!b) return;
    memcpy(b, src, len);
    pthread_mutex_lock(&qm);
    if (qcount == QN) {
        // Prefer to lose a frame of video over a block of audio: a dropped
        // frame is invisible, a gap in the sound is not.
        int drop = -1;
        for (int i = 0; i < qcount; i++)
            if (q[(qhead + i) % QN].type == HD60S_VIDEO) { drop = i; break; }
        if (drop < 0) drop = 0;
        free(q[(qhead + drop) % QN].data);
        for (int i = drop; i < qcount - 1; i++)
            q[(qhead + i) % QN] = q[(qhead + i + 1) % QN];
        qcount--; nDrop++;
    }
    q[(qhead + qcount) % QN] = (struct pkt){ b, len, ts, type };
    qcount++;
    pthread_cond_signal(&qc);
    pthread_mutex_unlock(&qm);
}

// Video and audio come off the same pixel clock: 800 stereo samples per frame,
// exactly. Counting them gives timestamps with no jitter, so the two streams
// stay locked to each other whatever the USB transfers do.
static void flushAudio(void) {
    if (!agot) return;
    uint64_t ts = sampleIdx * 1000000000ull / HD60S_RATE;
    queue(HD60S_AUDIO, abuf, (uint32_t)agot, ts);
    sampleIdx += agot / 4;
    agot = 0;
}

static int activeRun;

static void emitFrame(void) {
    if (activeRun <= 0) return;
    if (activeRun < ACTIVE) {                       // a dropped line: pad black
        nShort++;
        for (int y = activeRun; y < ACTIVE; y++)
            for (int x = 0; x < VID_BYTES; x += 2) {
                frame[(size_t)y * VID_BYTES + x]     = 0x10;
                frame[(size_t)y * VID_BYTES + x + 1] = 0x80;
            }
    }
    queue(HD60S_VIDEO, frame, FRAME_SZ, frameIdx * 1000000000ull / HD60S_FPS);
    frameIdx++;
    nFrame++;
    activeRun = 0;
}

static void putLine(int blanking) {
    nLine++;
    if (blanking) { nBlank++; emitFrame(); return; }
    if (activeRun < ACTIVE)
        memcpy(frame + (size_t)activeRun * VID_BYTES, line, VID_BYTES);
    activeRun++;
    if (activeRun == ACTIVE) emitFrame();
}

static void putAudio(const unsigned char *p) {
    nAud++;
    memcpy(abuf + agot, p, 8);
    agot += 8;
    if (agot == ACHUNK) flushAudio();
}

// ------------------------------------------------------------------ parsing

// A line trailer always starts ff 00 and the two bytes after it are either the
// end word (00 80 / 00 ab) or the audio sub-marker ff 02.
static int isMark(const unsigned char *p, size_t avail) {
    if (avail < 4) return -1;                       // undecidable yet
    if (p[0] != 0xff || p[1] != 0x00) return 0;
    if (p[2] == 0x00 && (p[3] == 0x80 || p[3] == 0xab)) return 1;
    if (p[2] == 0xff && p[3] == 0x02) return 1;
    return 0;
}

// ------------------------------------------------------------------- setup

// Opening a fifo write-only blocks until a reader arrives, which deadlocks
// against an ffmpeg that is still probing its first input. O_RDWR on a fifo
// never blocks on BSD, so the pipeline can be started in any order.
static FILE *openOut(const char *path) {
    struct stat st;
    if (!path) return NULL;
    if (!strcmp(path, "-")) return stdout;
    if (!stat(path, &st) && S_ISFIFO(st.st_mode)) {
        int fd = open(path, O_RDWR);
        if (fd < 0) { perror(path); exit(1); }
        return fdopen(fd, "wb");
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    return f;
}

// The plugin listens, we connect, so OBS may be started in either order. Wait
// for it rather than failing: the capture is usually started first.
static int connectObs(const char *path, int waitSecs) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    for (int i = 0; i < waitSecs * 4; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); exit(1); }
        if (!connect(fd, (struct sockaddr *)&sa, sizeof sa)) {
            fprintf(stderr, "demux: connected to the OBS plugin on %s\n", path);
            return fd;
        }
        close(fd);
        if (!i) fprintf(stderr, "demux: waiting for the OBS plugin on %s\n", path);
        usleep(250000);
    }
    fprintf(stderr, "demux: no OBS plugin on %s. Add the HD60 S source in OBS.\n", path);
    exit(1);
}

int main(int argc, char **argv) {
    const char *vp = NULL, *ap = NULL, *sp = NULL;
    int waitSecs = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--video") && i + 1 < argc) vp = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) ap = argv[++i];
        else if (!strcmp(argv[i], "--obs")) {
            sp = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : HD60S_SOCK_DEFAULT;
        } else if (!strcmp(argv[i], "--wait") && i + 1 < argc) waitSecs = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: demux [--video PATH] [--audio PATH] "
                            "[--obs [SOCKET]] [--wait SECS] < raw\n");
            return 1;
        }
    }
    signal(SIGINT, onint);
    signal(SIGPIPE, SIG_IGN);
    if (sp) sock = connectObs(sp, waitSecs);
    else {
        fv = openOut(vp);
        fa = openOut(ap);
    }

    pthread_t oth;
    pthread_create(&oth, NULL, outThread, NULL);

    // One line plus its trailer is under 4 KB; 64 KB of slack is ample and lets
    // us always decide isMark() without a second read.
    const size_t CAP = 1 << 16;
    unsigned char *buf = malloc(CAP);
    size_t have = 0;

    enum { SYNC, MARK, VIDEO } st = SYNC;
    int pendingBlank = 0;
    uint64_t t0 = nowNs(), reported = 0;

    while (!stop) {
        size_t room = CAP - have;
        if (room < 8192) { fprintf(stderr, "demux: parser stuck\n"); break; }
        ssize_t n = read(0, buf + have, room);
        if (n <= 0) break;
        have += (size_t)n;

        size_t p = 0;
        for (;;) {
            if (st == SYNC) {
                while (p + 4 <= have && isMark(buf + p, have - p) != 1) p += 2;
                if (p + 4 > have) break;
                nResync++;
                st = MARK;
            }
            if (st == MARK) {
                size_t r = p;
                if (have - r < 4) break;
                r += 2;                                      // ff 00
                int bad = 0;
                while (buf[r] == 0xff && buf[r + 1] == 0x02) {
                    if (have - r < 12) { bad = -1; break; }  // need more input
                    putAudio(buf + r + 2);
                    r += 10;
                    if (buf[r] == 0xff && buf[r + 1] == 0x00) r += 2;
                    else { bad = 1; break; }
                }
                if (bad < 0) break;
                if (bad > 0) { p += 2; st = SYNC; continue; }
                if (have - r < 2) break;
                pendingBlank = (buf[r + 1] == 0xab);
                r += 2;
                p = r;
                st = VIDEO;
            }
            if (st == VIDEO) {
                // Consume the line only once the whole of it plus the next
                // marker is in the buffer, so a line is never half-committed
                // across two reads.
                if (have - p < VID_BYTES + 4) break;
                if (isMark(buf + p + VID_BYTES, have - p - VID_BYTES) != 1) {
                    p += 2; st = SYNC; continue;
                }
                memcpy(line, buf + p, VID_BYTES);
                p += VID_BYTES;
                putLine(pendingBlank);
                st = MARK;
            }
        }
        memmove(buf, buf + p, have - p);
        have -= p;

        uint64_t el = (nowNs() - t0) / 1000000000ull;
        if (el != reported && el % 10 == 0) {
            reported = el;
            fprintf(stderr, "demux: %llu frames, %llu audio blocks, %llu dropped\n",
                    nFrame, nAud, nDrop);
        }
    }

    flushAudio();
    pthread_mutex_lock(&qm); qdone = 1; pthread_cond_signal(&qc); pthread_mutex_unlock(&qm);
    pthread_join(oth, NULL);
    if (sock >= 0) close(sock);
    if (fv && fv != stdout) fclose(fv);
    if (fa && fa != stdout) fclose(fa);
    fprintf(stderr, "demux: %llu lines (%llu blanking), %llu frames, %llu audio blocks "
                    "(%llu samples), %llu resyncs, %llu short frames, %llu dropped\n",
            nLine, nBlank, nFrame, nAud, nAud * 2, nResync, nShort, nDrop);
    return 0;
}
