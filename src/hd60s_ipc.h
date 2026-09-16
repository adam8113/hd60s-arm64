// framing between demux and the OBS plugin.
//
// demux connects to a unix socket that the plugin listens on, so OBS can be
// started before or after the capture. One packet header, then the payload.
// Timestamps are nanoseconds from the first packet of the connection; the
// plugin adds its own os_gettime_ns() base, which keeps the two processes out
// of each other's clock domain.
#pragma once
#include <stdint.h>

#define HD60S_SOCK_DEFAULT "/tmp/hd60s.sock"
#define HD60S_MAGIC 0x48363053u          // 'H60S'

#define HD60S_VIDEO 0
#define HD60S_AUDIO 1

#define HD60S_W 1920
#define HD60S_H 1080
#define HD60S_FPS 60
#define HD60S_RATE 48000

struct hd60s_hdr {
    uint32_t magic;
    uint8_t  type;
    uint8_t  pad[3];
    uint32_t len;                        // payload bytes following
    uint64_t ts;                         // ns since the first packet
};
