/* Real multimedia (sceAudio WinMM host sink, sceAtrac3plus & sceMpeg via FFmpeg)
 * and remaining non-server EBOOT firmware HLE endpoints.
 */

#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <psprecomp/recomp_rt.h>

#include "gl_present.h"
#include "seto_os.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#include <mmsystem.h>
#else
#include <SDL2/SDL.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * 1. WinMM Host Audio Output (sceAudio)
 * ============================================================================ */

#define AUDIO_CHANNELS   8
#define AUDIO_HDR_COUNT  16
#define AUDIO_BUF_BYTES  (4096u * 4u)
#define AUDIO_VOL_UNITY  0x8000

typedef struct {
    int        reserved;
    uint32_t   samples;
    uint32_t   format;     /* 0x00 = stereo S16, 0x10 = mono S16 */
    int32_t    vol_l;
    int32_t    vol_r;
    uint64_t   due_us;

#ifdef _WIN32
    HWAVEOUT   hwo;
    WAVEHDR    hdrs[AUDIO_HDR_COUNT];
    uint8_t   *bufs[AUDIO_HDR_COUNT];
    int        hdr_init;
    int        next_hdr;
#else
    SDL_AudioDeviceID dev;
    int16_t   *mix_buf;
    uint32_t   mix_cap_samples;
#endif
} audio_ch_state;

static audio_ch_state g_aud[AUDIO_CHANNELS];
static seto_mutex_t g_aud_cs;
static int g_aud_cs_init;

static int audio_headless(void) {
    static int cached = -1;
    if (cached < 0) cached = (getenv("SETO_NO_WINDOW") != NULL);
    return cached;
}

#ifdef _WIN32
static void audio_close_ch(audio_ch_state *c) {
    if (!c->hwo) return;
    waveOutReset(c->hwo);
    for (int i = 0; i < AUDIO_HDR_COUNT; i++) {
        if (c->hdr_init && (c->hdrs[i].dwFlags & WHDR_PREPARED))
            waveOutUnprepareHeader(c->hwo, &c->hdrs[i], sizeof(WAVEHDR));
        free(c->bufs[i]);
        c->bufs[i] = NULL;
    }
    waveOutClose(c->hwo);
    c->hwo = NULL;
    c->hdr_init = 0;
    c->next_hdr = 0;
}

static int audio_ensure_open(audio_ch_state *c) {
    if (audio_headless()) return 0;
    if (c->hwo) return 1;

    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof wfx);
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = 44100u * 4u;

    if (waveOutOpen(&c->hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        c->hwo = NULL;
        return 0;
    }
    for (int i = 0; i < AUDIO_HDR_COUNT; i++) {
        c->bufs[i] = (uint8_t *)calloc(1, AUDIO_BUF_BYTES);
        memset(&c->hdrs[i], 0, sizeof(WAVEHDR));
        if (!c->bufs[i]) {
            audio_close_ch(c);
            return 0;
        }
        c->hdrs[i].lpData = (LPSTR)c->bufs[i];
        c->hdrs[i].dwBufferLength = AUDIO_BUF_BYTES;
        waveOutPrepareHeader(c->hwo, &c->hdrs[i], sizeof(WAVEHDR));
        c->hdrs[i].dwFlags |= WHDR_DONE;
    }
    c->hdr_init = 1;
    c->next_hdr = 0;
    return 1;
}
#else
static int g_sdl_aud_init;

static void audio_close_ch(audio_ch_state *c) {
    if (c->dev) {
        SDL_ClearQueuedAudio(c->dev);
        SDL_CloseAudioDevice(c->dev);
        c->dev = 0;
    }
    free(c->mix_buf);
    c->mix_buf = NULL;
    c->mix_cap_samples = 0;
}

static int audio_ensure_open(audio_ch_state *c) {
    if (audio_headless()) return 0;
    if (c->dev) return 1;
    if (!g_sdl_aud_init) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 0;
        g_sdl_aud_init = 1;
    }
    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = 44100;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    c->dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!c->dev) return 0;
    SDL_PauseAudioDevice(c->dev, 0);
    return 1;
}
#endif

static int16_t clamp_s16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void audio_submit_pcm(int ch, uint32_t buf_addr, int32_t vl, int32_t vr, int blocking) {
    if (ch < 0 || ch >= AUDIO_CHANNELS || !buf_addr) return;
    audio_ch_state *c = &g_aud[ch];
    uint32_t samples = c->samples;
    if (!samples || samples > 4096u) return;

    uint32_t in_bytes = samples * (c->format == 0x10u ? 2u : 4u);
    int16_t raw[4096 * 2];
    if (psp_mem_read_block(raw, buf_addr, in_bytes) != 0) return;

    const int max_q = (samples >= 1024u) ? 4 : 8;

#ifdef _WIN32
    seto_mutex_lock(&g_aud_cs);
    if (!audio_ensure_open(c)) {
        seto_mutex_unlock(&g_aud_cs);
        return;
    }

    WAVEHDR *hdr = &c->hdrs[c->next_hdr];
    for (int wait = 0; wait < 500; wait++) {
        int in_flight = 0;
        for (int i = 0; i < AUDIO_HDR_COUNT; i++) {
            if (!(c->hdrs[i].dwFlags & WHDR_DONE)) in_flight++;
        }
        if ((hdr->dwFlags & WHDR_DONE) && in_flight < max_q) break;
        if (!blocking) {
            seto_mutex_unlock(&g_aud_cs);
            return;
        }
        /* Release both the audio lock and the CPU baton while WinMM plays out
         * a queued buffer, keeping audio locked to 44.1 kHz without stalling video. */
        seto_mutex_unlock(&g_aud_cs);
        psp_threadman_host_sleep_ms(1);
        seto_mutex_lock(&g_aud_cs);
        if (!c->reserved || !c->hwo) {
            seto_mutex_unlock(&g_aud_cs);
            return;
        }
        hdr = &c->hdrs[c->next_hdr];
    }
    if (!(hdr->dwFlags & WHDR_DONE)) {
        seto_mutex_unlock(&g_aud_cs);
        return;
    }

    int16_t *dst = (int16_t *)hdr->lpData;
#else
    seto_mutex_lock(&g_aud_cs);
    if (!audio_ensure_open(c)) {
        seto_mutex_unlock(&g_aud_cs);
        return;
    }
    const uint32_t max_q_bytes = (uint32_t)max_q * samples * 4u;
    for (int wait = 0; wait < 500; wait++) {
        if (SDL_GetQueuedAudioSize(c->dev) < max_q_bytes) break;
        if (!blocking) {
            seto_mutex_unlock(&g_aud_cs);
            return;
        }
        seto_mutex_unlock(&g_aud_cs);
        psp_threadman_host_sleep_ms(1);
        seto_mutex_lock(&g_aud_cs);
        if (!c->reserved || !c->dev) {
            seto_mutex_unlock(&g_aud_cs);
            return;
        }
    }
    if (SDL_GetQueuedAudioSize(c->dev) >= max_q_bytes * 2u) {
        seto_mutex_unlock(&g_aud_cs);
        return;
    }
    if (samples > c->mix_cap_samples) {
        int16_t *nb = (int16_t *)realloc(c->mix_buf, (size_t)samples * 4u);
        if (!nb) {
            seto_mutex_unlock(&g_aud_cs);
            return;
        }
        c->mix_buf = nb;
        c->mix_cap_samples = samples;
    }
    int16_t *dst = c->mix_buf;
#endif

    if (c->format == 0x10u) {
        for (uint32_t i = 0; i < samples; i++) {
            int32_t s = raw[i];
            dst[i * 2 + 0] = clamp_s16((s * vl) >> 15);
            dst[i * 2 + 1] = clamp_s16((s * vr) >> 15);
        }
    } else {
        for (uint32_t i = 0; i < samples; i++) {
            int32_t l = raw[i * 2 + 0];
            int32_t r = raw[i * 2 + 1];
            dst[i * 2 + 0] = clamp_s16((l * vl) >> 15);
            dst[i * 2 + 1] = clamp_s16((r * vr) >> 15);
        }
    }

#ifdef _WIN32
    hdr->dwFlags &= ~WHDR_DONE;
    hdr->dwBufferLength = samples * 4u;
    waveOutWrite(c->hwo, hdr, sizeof(WAVEHDR));
    c->next_hdr = (c->next_hdr + 1) % AUDIO_HDR_COUNT;
#else
    SDL_QueueAudio(c->dev, dst, samples * 4u);
#endif
    seto_mutex_unlock(&g_aud_cs);
}

static void hle_audio_ch_reserve(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0) {
        for (int i = 0; i < AUDIO_CHANNELS; i++) {
            if (!g_aud[i].reserved) { ch = i; break; }
        }
    }
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001u); return; }
    g_aud[ch].reserved = 1;
    g_aud[ch].samples = psp_arg(1);
    g_aud[ch].format  = psp_arg(2);
    g_aud[ch].vol_l   = AUDIO_VOL_UNITY;
    g_aud[ch].vol_r   = AUDIO_VOL_UNITY;
    g_aud[ch].due_us  = 0;
    psp_ret((uint32_t)ch);
}

static void hle_audio_ch_release(void) {
    int32_t ch = (int32_t)psp_arg(0);
    if (ch < 0 || ch >= AUDIO_CHANNELS) { psp_ret(0x80000001u); return; }
    seto_mutex_lock(&g_aud_cs);
    audio_close_ch(&g_aud[ch]);
    seto_mutex_unlock(&g_aud_cs);
    g_aud[ch].reserved = 0;
    psp_ret(0);
}

static void hle_audio_set_len(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003u); return; }
    g_aud[ch].samples = psp_arg(1);
    psp_ret(0);
}

static void hle_audio_change_vol(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0x80260003u); return; }
    int32_t vl = (int32_t)psp_arg(1);
    int32_t vr = (int32_t)psp_arg(2);
    if (vl >= 0 && vl <= 0x10000) g_aud[ch].vol_l = vl;
    if (vr >= 0 && vr <= 0x10000) g_aud[ch].vol_r = vr;
    psp_ret(0);
}

static void hle_audio_output_panned(void) {
    uint32_t ch = psp_arg(0);
    if (ch >= AUDIO_CHANNELS) { psp_ret(0); return; }
    int32_t vl = (int32_t)psp_arg(1);
    int32_t vr = (int32_t)psp_arg(2);
    if (vl < 0 || vl > 0x10000) vl = g_aud[ch].vol_l;
    if (vr < 0 || vr > 0x10000) vr = g_aud[ch].vol_r;
    audio_submit_pcm((int)ch, psp_arg(3), vl, vr, 0);
    psp_ret(g_aud[ch].samples);
}

static void hle_audio_output_blocking(int panned) {
    uint32_t ch = psp_arg(0);
    uint32_t samples = ch < AUDIO_CHANNELS ? g_aud[ch].samples : 0;
    if (ch < AUDIO_CHANNELS) {
        int32_t vl = g_aud[ch].vol_l, vr = g_aud[ch].vol_r;
        uint32_t buf_addr;
        if (panned) {
            int32_t al = (int32_t)psp_arg(1), ar = (int32_t)psp_arg(2);
            if (al >= 0 && al <= 0x10000) vl = al;
            if (ar >= 0 && ar <= 0x10000) vr = ar;
            buf_addr = psp_arg(3);
        } else {
            int32_t av = (int32_t)psp_arg(1);
            if (av >= 0 && av <= 0x10000) vl = vr = av;
            buf_addr = psp_arg(2);
        }
        audio_submit_pcm((int)ch, buf_addr, vl, vr, 1);

        const uint64_t play_us = (uint64_t)samples * 1000000ull / 44100ull;
        uint64_t vnow = psp_threadman_vtime_us();
        if (!g_aud[ch].due_us || g_aud[ch].due_us + 12u * play_us < vnow)
            g_aud[ch].due_us = vnow;
        g_aud[ch].due_us += play_us;
        if (g_aud[ch].due_us > vnow) psp_threadman_delay(g_aud[ch].due_us - vnow);
    }
    psp_ret(samples);
}

static void hle_audio_blocking_unpanned(void) { hle_audio_output_blocking(0); }
static void hle_audio_blocking_panned(void)   { hle_audio_output_blocking(1); }

/* ============================================================================
 * 2. Real sceAtrac3plus Decoding via FFmpeg
 * ============================================================================ */

#define MAX_ATRAC_IDS 6
#define SCE_ERROR_ATRAC_NO_ATRACID        0x80630003u
#define SCE_ERROR_ATRAC_BAD_ATRACID       0x80630005u
#define SCE_ERROR_ATRAC_UNKNOWN_FORMAT    0x80630006u
#define SCE_ERROR_ATRAC_INVALID_SIZE      0x80630010u
#define SCE_ERROR_ATRAC_BAD_SAMPLE        0x80630015u
#define SCE_ERROR_ATRAC_NO_LOOP_INFO      0x80630021u
#define SCE_ERROR_ATRAC_ALL_DATA_DECODED  0x80630024u

#define WAVE_FORMAT_ATRAC3     0x0270u
#define WAVE_FORMAT_ATRAC3PLUS 0xFFFEu

typedef struct {
    int      used;
    uint16_t channels;
    uint16_t bytes_per_frame;
    uint32_t samples_per_frame;
    uint32_t current_sample;
    uint32_t end_sample;
    int32_t  loop_start;
    int32_t  loop_end;
    int32_t  loop_num;
    int      finished;
    int16_t *pcm;
    uint32_t pcm_samples;
} atrac_dec_slot;

static atrac_dec_slot g_atrac[MAX_ATRAC_IDS];

typedef struct {
    const uint8_t *buf;
    size_t         size;
    size_t         pos;
} mem_avio_reader;

static int mem_avio_read(void *opaque, uint8_t *dst, int len) {
    mem_avio_reader *r = (mem_avio_reader *)opaque;
    if (r->pos >= r->size) return AVERROR_EOF;
    size_t avail = r->size - r->pos;
    size_t n = (size_t)len < avail ? (size_t)len : avail;
    memcpy(dst, r->buf + r->pos, n);
    r->pos += n;
    return (int)n;
}

static int64_t mem_avio_seek(void *opaque, int64_t off, int whence) {
    mem_avio_reader *r = (mem_avio_reader *)opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)r->size;
    int64_t next = (whence == SEEK_SET) ? off :
                   (whence == SEEK_CUR) ? (int64_t)r->pos + off :
                   (whence == SEEK_END) ? (int64_t)r->size + off : -1;
    if (next < 0 || (size_t)next > r->size) return -1;
    r->pos = (size_t)next;
    return next;
}

static void decode_atrac_buffer(const uint8_t *raw, uint32_t size, uint32_t skip_samples,
                                uint16_t want_ch, int16_t **out_pcm, uint32_t *out_samples) {
    *out_pcm = NULL;
    *out_samples = 0;

    mem_avio_reader mr = { raw, size, 0 };
    uint8_t *io_buf = (uint8_t *)av_malloc(16384);
    if (!io_buf) return;

    AVIOContext *avio = avio_alloc_context(io_buf, 16384, 0, &mr, mem_avio_read, NULL, mem_avio_seek);
    if (!avio) { av_free(io_buf); return; }

    AVFormatContext *fmt = avformat_alloc_context();
    fmt->pb = avio;
    if (avformat_open_input(&fmt, "mem.at3", NULL, NULL) < 0) {
        avformat_free_context(fmt);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return;
    }

    if (avformat_find_stream_info(fmt, NULL) < 0) goto done_fmt;

    int aidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            aidx = (int)i;
            break;
        }
    }
    if (aidx < 0) goto done_fmt;

    const AVCodecParameters *par = fmt->streams[aidx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) goto done_fmt;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) goto done_fmt;
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        goto done_fmt;
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, want_ch);
    SwrContext *swr = NULL;
    swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, 44100,
                        &ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (!swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&ctx);
        goto done_fmt;
    }

    uint32_t cap_samples = 65536;
    uint32_t total_samples = 0;
    int16_t *pcm = (int16_t *)malloc((size_t)cap_samples * want_ch * sizeof(int16_t));
    AVPacket *pkt = av_packet_alloc();
    AVFrame *avf = av_frame_alloc();

    if (pcm && pkt && avf) {
        for (int flush = 0; flush < 2; flush++) {
            for (;;) {
                int r = flush ? 0 : av_read_frame(fmt, pkt);
                if (!flush && r < 0) break;
                if (!flush && pkt->stream_index != aidx) { av_packet_unref(pkt); continue; }
                avcodec_send_packet(ctx, flush ? NULL : pkt);
                if (!flush) av_packet_unref(pkt);
                while (avcodec_receive_frame(ctx, avf) == 0) {
                    int out_n = swr_get_out_samples(swr, avf->nb_samples);
                    if (total_samples + (uint32_t)out_n > cap_samples) {
                        uint32_t ncap = cap_samples * 2u;
                        while (ncap < total_samples + (uint32_t)out_n) ncap *= 2u;
                        int16_t *np = (int16_t *)realloc(pcm, (size_t)ncap * want_ch * sizeof(int16_t));
                        if (!np) break;
                        pcm = np;
                        cap_samples = ncap;
                    }
                    uint8_t *dst_planes[1] = { (uint8_t *)(pcm + (size_t)total_samples * want_ch) };
                    int conv = swr_convert(swr, dst_planes, out_n,
                                           (const uint8_t **)avf->extended_data, avf->nb_samples);
                    if (conv > 0) total_samples += (uint32_t)conv;
                }
                if (flush) break;
            }
        }
        if (skip_samples > 0 && total_samples > skip_samples) {
            uint32_t rem = total_samples - skip_samples;
            memmove(pcm, pcm + (size_t)skip_samples * want_ch, (size_t)rem * want_ch * sizeof(int16_t));
            total_samples = rem;
        }
        *out_pcm = pcm;
        *out_samples = total_samples;
    } else {
        free(pcm);
    }

    av_frame_free(&avf);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&ctx);

done_fmt:
    avformat_close_input(&fmt);
    av_freep(&avio->buffer);
    avio_context_free(&avio);
}

static atrac_dec_slot *atrac_slot_get(int id) {
    if (id < 0 || id >= MAX_ATRAC_IDS || !g_atrac[id].used) return NULL;
    return &g_atrac[id];
}

static void atrac_free_slot(atrac_dec_slot *s) {
    free(s->pcm);
    memset(s, 0, sizeof *s);
}

static void atrac_reset_all(void) {
    for (int i = 0; i < MAX_ATRAC_IDS; i++) atrac_free_slot(&g_atrac[i]);
}

static void hle_atrac_reinit(void) {
    atrac_reset_all();
    psp_ret(0);
}

static void hle_atrac_set_data_and_get_id(void) {
    const uint32_t buf  = psp_arg(0);
    const uint32_t size = psp_arg(1);
    if (!buf || size < 44u) { psp_ret(SCE_ERROR_ATRAC_INVALID_SIZE); return; }
    if (psp_read32(buf + 0) != 0x46464952u || psp_read32(buf + 8) != 0x45564157u) {
        psp_ret(SCE_ERROR_ATRAC_UNKNOWN_FORMAT);
        return;
    }

    int have_fmt = 0;
    uint16_t channels = 2, bpf = 0;
    uint32_t spf = 0, fact_samples = 0, fact_offset = 0, data_size = 0;
    int32_t loop_start = -1, loop_end = -1;

    uint32_t pos = 12;
    while (pos + 8u <= size) {
        const uint32_t cid = psp_read32(buf + pos);
        const uint32_t csz = psp_read32(buf + pos + 4);
        const uint32_t pay = buf + pos + 8u;
        const uint32_t avail = size - (pos + 8u);
        const uint32_t len = csz < avail ? csz : avail;

        if (cid == 0x20746D66u && len >= 16u) {
            const uint16_t tag = psp_read16(pay + 0);
            channels = psp_read16(pay + 2);
            if (channels == 0 || channels > 2) channels = 2;
            bpf = psp_read16(pay + 12);
            if (tag == WAVE_FORMAT_ATRAC3) { spf = 1024u; have_fmt = 1; }
            else if (tag == WAVE_FORMAT_ATRAC3PLUS) { spf = 2048u; have_fmt = 1; }
        } else if (cid == 0x74636166u && len >= 4u) {
            fact_samples = psp_read32(pay + 0);
            if (len >= 8u) fact_offset = psp_read32(pay + 4);
        } else if (cid == 0x6C706D73u && len >= 60u) {
            const uint32_t nloops = psp_read32(pay + 28);
            if (nloops > 0) {
                loop_start = (int32_t)psp_read32(pay + 36 + 8);
                loop_end   = (int32_t)psp_read32(pay + 36 + 12);
            }
        } else if (cid == 0x61746164u) {
            data_size = csz;
        }
        const uint64_t next = (uint64_t)pos + 8ull + (((uint64_t)csz + 1ull) & ~1ull);
        if (next <= pos || next > size) break;
        pos = (uint32_t)next;
    }

    if (!have_fmt) { psp_ret(SCE_ERROR_ATRAC_UNKNOWN_FORMAT); return; }

    int id = -1;
    for (int i = 0; i < MAX_ATRAC_IDS; i++) {
        if (!g_atrac[i].used) { id = i; break; }
    }
    if (id < 0) { psp_ret(SCE_ERROR_ATRAC_NO_ATRACID); return; }

    uint32_t end_sample = fact_samples;
    if (end_sample == 0 && bpf > 0) end_sample = (data_size / bpf) * spf;
    if (end_sample == 0) end_sample = spf;

    if (loop_start >= 0) {
        if (fact_offset > 0) {
            loop_start -= (int32_t)fact_offset;
            loop_end   -= (int32_t)fact_offset;
            if (loop_start < 0) loop_start = 0;
            if (loop_end < 0) loop_end = 0;
        }
        if (loop_end >= (int32_t)end_sample)
            loop_end = (int32_t)end_sample - 1;
    }

    atrac_dec_slot *s = &g_atrac[id];
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->channels = channels;
    s->bytes_per_frame = bpf;
    s->samples_per_frame = spf;
    s->current_sample = 0;
    s->end_sample = end_sample;
    s->loop_start = loop_start;
    s->loop_end = loop_end;

    uint8_t *raw = (uint8_t *)malloc(size);
    if (raw) {
        if (psp_mem_read_block(raw, buf, size) == 0)
            decode_atrac_buffer(raw, size, fact_offset, channels, &s->pcm, &s->pcm_samples);
        free(raw);
    }
    psp_ret((uint32_t)id);
}

static void hle_atrac_release(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    atrac_free_slot(s);
    psp_ret(0);
}

static void hle_atrac_get_max_sample(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    uint32_t out = psp_arg(1);
    if (out) psp_write32(out, s->samples_per_frame);
    psp_ret(0);
}

static void hle_atrac_get_sound_sample(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    uint32_t p_end = psp_arg(1), p_ls = psp_arg(2), p_le = psp_arg(3);
    if (p_end) psp_write32(p_end, s->end_sample > 0 ? s->end_sample - 1u : 0u);
    if (p_ls)  psp_write32(p_ls, (uint32_t)s->loop_start);
    if (p_le)  psp_write32(p_le, (uint32_t)s->loop_end);
    psp_ret(0);
}

static void hle_atrac_set_loop_num(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    int32_t nloops = (int32_t)psp_arg(1);
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    if (s->loop_start < 0 && nloops != 0) { psp_ret(SCE_ERROR_ATRAC_NO_LOOP_INFO); return; }
    s->loop_num = nloops;
    psp_ret(0);
}

static void hle_atrac_reset_pos(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    uint32_t sample = psp_arg(1);
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    if (sample >= s->end_sample) { psp_ret(SCE_ERROR_ATRAC_BAD_SAMPLE); return; }
    s->current_sample = sample;
    s->finished = 0;
    psp_ret(0);
}

static void hle_atrac_get_loop_status(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }
    uint32_t p_num = psp_arg(1), p_status = psp_arg(2);
    if (p_num) psp_write32(p_num, (uint32_t)s->loop_num);
    if (p_status) psp_write32(p_status, (s->loop_start >= 0 && s->loop_num != 0) ? 1u : 0u);
    psp_ret(0);
}

static void hle_atrac_decode_data(void) {
    atrac_dec_slot *s = atrac_slot_get((int)psp_arg(0));
    uint32_t out_buf = psp_arg(1), out_num = psp_arg(2);
    uint32_t out_end = psp_arg(3), out_remain = psp_arg(4);
    if (!s) { psp_ret(SCE_ERROR_ATRAC_BAD_ATRACID); return; }

    if (s->finished) {
        if (out_num)    psp_write32(out_num, 0);
        if (out_end)    psp_write32(out_end, 1);
        if (out_remain) psp_write32(out_remain, 0);
        psp_threadman_yield();
        psp_ret(SCE_ERROR_ATRAC_ALL_DATA_DECODED);
        return;
    }

    uint32_t limit = s->end_sample;
    int looping = (s->loop_num != 0 && s->loop_start >= 0 && s->loop_end >= s->loop_start);
    if (looping && (uint32_t)s->loop_end + 1u < limit)
        limit = (uint32_t)s->loop_end + 1u;

    uint32_t avail = limit > s->current_sample ? limit - s->current_sample : 0;
    uint32_t n = s->samples_per_frame < avail ? s->samples_per_frame : avail;

    if (out_buf && n > 0) {
        uint32_t bytes = n * s->channels * 2u;
        uint32_t copied = 0;
        if (s->pcm && s->current_sample < s->pcm_samples) {
            uint32_t have = s->pcm_samples - s->current_sample;
            if (have > n) have = n;
            uint32_t cbytes = have * s->channels * 2u;
            psp_mem_write_block(out_buf, s->pcm + (size_t)s->current_sample * s->channels, cbytes);
            copied = cbytes;
        }
        for (uint32_t i = copied; i < bytes; i++) psp_write8(out_buf + i, 0);
    }

    s->current_sample += n;
    int end_flag = 0;
    if (s->current_sample >= limit) {
        if (looping) {
            if (s->loop_num > 0) s->loop_num--;
            s->current_sample = (uint32_t)s->loop_start;
        } else {
            s->finished = 1;
            end_flag = 1;
        }
    }

    if (out_num) psp_write32(out_num, n);
    if (out_end) psp_write32(out_end, (uint32_t)end_flag);
    if (out_remain) {
        uint32_t rem = 0;
        if (!end_flag && s->end_sample > s->current_sample)
            rem = (s->end_sample - s->current_sample + s->samples_per_frame - 1u) / s->samples_per_frame;
        psp_write32(out_remain, rem);
    }
    psp_ret(0);
}

/* ============================================================================
 * 3. Real sceMpeg (.pmf PSMF Video H.264 + Audio ATRAC3+ via FFmpeg)
 * ============================================================================ */

#define SCE_MPEG_ERROR_NO_DATA 0x80618001u
#define MPEG_AU_CAP            512
#define MPEG_RB_HOST_CAP       (4u * 1024u * 1024u)

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t pts_hi;
    uint32_t pts_lo;
} mpeg_au_item;

typedef struct {
    uint8_t       *buf;
    size_t         len;
    size_t         cap;
    mpeg_au_item   q[MPEG_AU_CAP];
    int            q_head, q_tail, q_count;
    size_t         queued_bytes;
    uint64_t       cur_pts;
} mpeg_es_stream;

typedef struct {
    int            active;
    uint32_t       rb_addr;
    int32_t        rb_packets;
    uint32_t       rb_data_addr;
    uint32_t       rb_cb_addr;
    uint32_t       rb_cb_arg;

    uint32_t       stream_size;
    uint32_t       stream_fed;
    int            eof;

    uint8_t       *ps_buf;
    size_t         ps_len;
    size_t         ps_cap;

    mpeg_es_stream v_es;
    mpeg_es_stream a_es;

    uint8_t       *cur_v_data;
    uint32_t       cur_v_size;
    uint8_t       *cur_a_data;
    uint32_t       cur_a_size;

    AVCodecContext     *v_ctx;
    struct SwsContext  *sws;
    AVFrame            *v_frame;
    AVFrame            *rgba_frame;
    int                 sws_w, sws_h;

    AVCodecContext     *a_ctx;
    SwrContext         *swr;
    AVFrame            *a_frame;
    int                 a_block_align;
} mpeg_ctx;

static mpeg_ctx g_mpeg;

static void es_clear(mpeg_es_stream *es, int keep_cap) {
    for (int i = 0; i < es->q_count; i++) {
        int idx = (es->q_head + i) % MPEG_AU_CAP;
        free(es->q[idx].data);
        es->q[idx].data = NULL;
    }
    es->q_head = es->q_tail = es->q_count = 0;
    es->queued_bytes = 0;
    es->len = 0;
    es->cur_pts = 90000;
    if (!keep_cap) {
        free(es->buf);
        es->buf = NULL;
        es->cap = 0;
    }
}

static void es_append(mpeg_es_stream *es, const uint8_t *src, size_t n) {
    if (!n) return;
    if (es->len + n > es->cap) {
        size_t ncap = es->cap ? es->cap * 2u : 65536u;
        while (ncap < es->len + n) ncap *= 2u;
        uint8_t *nb = (uint8_t *)realloc(es->buf, ncap);
        if (!nb) return;
        es->buf = nb;
        es->cap = ncap;
    }
    memcpy(es->buf + es->len, src, n);
    es->len += n;
}

static void es_push_au(mpeg_es_stream *es, const uint8_t *src, uint32_t n, uint64_t pts) {
    if (!n || es->q_count >= MPEG_AU_CAP) return;
    /* Pad with AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes for safe FFmpeg bitstream reads. */
    uint8_t *copy = (uint8_t *)malloc((size_t)n + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!copy) return;
    memcpy(copy, src, n);
    memset(copy + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    mpeg_au_item *it = &es->q[es->q_tail];
    it->data = copy;
    it->size = n;
    it->pts_hi = (uint32_t)(pts >> 32);
    it->pts_lo = (uint32_t)(pts & 0xFFFFFFFFu);
    es->q_tail = (es->q_tail + 1) % MPEG_AU_CAP;
    es->q_count++;
    es->queued_bytes += n;
}

static int es_pop_au(mpeg_es_stream *es, uint8_t **out_data, uint32_t *out_size,
                     uint32_t *pts_hi, uint32_t *pts_lo) {
    if (es->q_count <= 0) return 0;
    mpeg_au_item *it = &es->q[es->q_head];
    free(*out_data);
    *out_data = it->data;
    *out_size = it->size;
    *pts_hi = it->pts_hi;
    *pts_lo = it->pts_lo;
    it->data = NULL;
    es->q_head = (es->q_head + 1) % MPEG_AU_CAP;
    es->q_count--;
    if (es->queued_bytes >= *out_size) es->queued_bytes -= *out_size;
    else es->queued_bytes = 0;
    return 1;
}

static void mpeg_free_codecs(void) {
    if (g_mpeg.sws) { sws_freeContext(g_mpeg.sws); g_mpeg.sws = NULL; }
    if (g_mpeg.v_frame) av_frame_free(&g_mpeg.v_frame);
    if (g_mpeg.rgba_frame) av_frame_free(&g_mpeg.rgba_frame);
    if (g_mpeg.v_ctx) avcodec_free_context(&g_mpeg.v_ctx);
    if (g_mpeg.swr) { swr_free(&g_mpeg.swr); g_mpeg.swr = NULL; }
    if (g_mpeg.a_frame) av_frame_free(&g_mpeg.a_frame);
    if (g_mpeg.a_ctx) avcodec_free_context(&g_mpeg.a_ctx);
    g_mpeg.sws_w = g_mpeg.sws_h = 0;
    g_mpeg.a_block_align = 0;
}

static void mpeg_reset_state(void) {
    mpeg_free_codecs();
    es_clear(&g_mpeg.v_es, 0);
    es_clear(&g_mpeg.a_es, 0);
    free(g_mpeg.ps_buf);
    free(g_mpeg.cur_v_data);
    free(g_mpeg.cur_a_data);
    memset(&g_mpeg, 0, sizeof g_mpeg);
    g_mpeg.v_es.cur_pts = 90000;
    g_mpeg.a_es.cur_pts = 90000;
}

static uint64_t parse_pes_pts(const uint8_t *p) {
    uint64_t pts = ((uint64_t)(p[0] & 0x0Eu) << 29)
                 | ((uint64_t)p[1] << 22)
                 | ((uint64_t)(p[2] & 0xFEu) << 14)
                 | ((uint64_t)p[3] << 7)
                 | ((uint64_t)(p[4] >> 1));
    return pts;
}

static size_t find_h264_aud(const uint8_t *b, size_t start, size_t len) {
    size_t i = start;
    while (i + 5 <= len) {
        const uint8_t *z = (const uint8_t *)memchr(b + i, 0, len - 4 - i);
        if (!z) break;
        size_t idx = (size_t)(z - b);
        if (b[idx + 1] == 0 && b[idx + 2] == 0 && b[idx + 3] == 1 && b[idx + 4] == 9)
            return idx;
        i = idx + 1;
    }
    return SIZE_MAX;
}

static void extract_video_aus(int flush_eof) {
    mpeg_es_stream *es = &g_mpeg.v_es;
    for (;;) {
        size_t a0 = find_h264_aud(es->buf, 0, es->len);
        if (a0 == SIZE_MAX) {
            if (flush_eof && es->len > 0) {
                es_push_au(es, es->buf, (uint32_t)es->len, es->cur_pts);
                es->cur_pts += 3003;
                es->len = 0;
            }
            break;
        }
        if (a0 > 0) {
            memmove(es->buf, es->buf + a0, es->len - a0);
            es->len -= a0;
        }
        size_t a1 = find_h264_aud(es->buf, 5, es->len);
        if (a1 == SIZE_MAX) {
            if (flush_eof && es->len > 0) {
                es_push_au(es, es->buf, (uint32_t)es->len, es->cur_pts);
                es->cur_pts += 3003;
                es->len = 0;
            }
            break;
        }
        es_push_au(es, es->buf, (uint32_t)a1, es->cur_pts);
        es->cur_pts += 3003;
        memmove(es->buf, es->buf + a1, es->len - a1);
        es->len -= a1;
    }
}

static void extract_audio_aus(void) {
    mpeg_es_stream *es = &g_mpeg.a_es;
    while (es->len >= 8u) {
        if (es->buf[0] != 0x0Fu || es->buf[1] != 0xD0u) {
            size_t i = 1;
            while (i + 1 < es->len && !(es->buf[i] == 0x0Fu && es->buf[i + 1] == 0xD0u)) i++;
            memmove(es->buf, es->buf + i, es->len - i);
            es->len -= i;
            continue;
        }
        uint32_t raw_len = ((((uint32_t)(es->buf[2] & 0x03u) << 8) | es->buf[3]) + 1u) * 8u;
        uint32_t frm_total = 8u + raw_len;
        if (es->len < frm_total) break;
        es_push_au(es, es->buf + 8u, raw_len, es->cur_pts);
        es->cur_pts += 4180;
        memmove(es->buf, es->buf + frm_total, es->len - frm_total);
        es->len -= frm_total;
    }
}

static void mpeg_demux_step(void) {
    size_t pos = 0;
    const uint8_t *b = g_mpeg.ps_buf;
    size_t len = g_mpeg.ps_len;

    while (pos + 6 <= len) {
        if (b[pos] != 0 || b[pos + 1] != 0 || b[pos + 2] != 1) {
            pos++;
            continue;
        }
        uint8_t sid = b[pos + 3];
        if (sid == 0xBAu) {
            if (pos + 14 > len) break;
            size_t pack_len = 14u + (size_t)(b[pos + 13] & 7u);
            if (pos + pack_len > len) break;
            pos += pack_len;
        } else if (sid == 0xB9u) { /* program end */
            g_mpeg.eof = 1;
            pos += 4;
        } else if (sid >= 0xBBu) {
            uint16_t plen = (uint16_t)(((uint16_t)b[pos + 4] << 8) | b[pos + 5]);
            size_t pkt_total = 6u + (size_t)plen;
            if (pos + pkt_total > len) break;

            if ((sid == 0xE0u || sid == 0xBDu) && plen >= 3u) {
                uint8_t flags = b[pos + 7];
                uint8_t hlen = b[pos + 8];
                size_t pay_off = pos + 9u + (size_t)hlen;
                if (pay_off <= pos + pkt_total) {
                    size_t pay_len = pos + pkt_total - pay_off;
                    if ((flags & 0x80u) && hlen >= 5u && pos + 14 <= len) {
                        uint64_t pts = parse_pes_pts(b + pos + 9);
                        if (sid == 0xE0u && g_mpeg.v_es.q_count == 0 && g_mpeg.v_es.len == 0)
                            g_mpeg.v_es.cur_pts = pts;
                        if (sid == 0xBDu && g_mpeg.a_es.q_count == 0 && g_mpeg.a_es.len == 0)
                            g_mpeg.a_es.cur_pts = pts;
                    }
                    if (sid == 0xE0u) {
                        es_append(&g_mpeg.v_es, b + pay_off, pay_len);
                    } else if (sid == 0xBDu && pay_len > 4u) {
                        es_append(&g_mpeg.a_es, b + pay_off + 4u, pay_len - 4u);
                    }
                }
            }
            pos += pkt_total;
        } else {
            pos++;
        }
    }

    if (pos > 0) {
        if (pos < g_mpeg.ps_len)
            memmove(g_mpeg.ps_buf, g_mpeg.ps_buf + pos, g_mpeg.ps_len - pos);
        g_mpeg.ps_len -= pos;
    }

    extract_video_aus(g_mpeg.eof);
    extract_audio_aus();
}

static uint32_t read_be32_guest(uint32_t addr) {
    return ((uint32_t)psp_read8(addr + 0) << 24)
         | ((uint32_t)psp_read8(addr + 1) << 16)
         | ((uint32_t)psp_read8(addr + 2) << 8)
         | (uint32_t)psp_read8(addr + 3);
}

static void hle_mpeg_init(void)   { mpeg_reset_state(); psp_ret(0); }
static void hle_mpeg_finish(void) { mpeg_reset_state(); psp_ret(0); }
static void hle_mpeg_query_mem_size(void) { psp_ret(0x10000u); }
static void hle_mpeg_rb_query_mem_size(void) {
    int32_t pkts = (int32_t)psp_arg(0);
    psp_ret((uint32_t)((pkts > 0 ? pkts : 512) * 2048 + 0x600));
}

static void hle_mpeg_rb_construct(void) {
    uint32_t rb = psp_arg(0);
    int32_t pkts = (int32_t)psp_arg(1);
    uint32_t data = psp_arg(2);
    uint32_t cb = psp_arg(4);
    uint32_t cb_arg = psp_arg(5);

    g_mpeg.rb_addr = rb;
    g_mpeg.rb_packets = pkts > 0 ? pkts : 512;
    g_mpeg.rb_data_addr = data;
    g_mpeg.rb_cb_addr = cb;
    g_mpeg.rb_cb_arg = cb_arg;

    if (rb) {
        psp_write32(rb + 0x00u, (uint32_t)g_mpeg.rb_packets);
        psp_write32(rb + 0x04u, 0);
        psp_write32(rb + 0x08u, 0);
        psp_write32(rb + 0x0Cu, (uint32_t)g_mpeg.rb_packets);
        psp_write32(rb + 0x10u, 2048u);
        psp_write32(rb + 0x14u, data);
        psp_write32(rb + 0x18u, cb);
        psp_write32(rb + 0x1Cu, cb_arg);
        psp_write32(rb + 0x20u, data + (uint32_t)g_mpeg.rb_packets * 2048u);
        psp_write32(rb + 0x24u, 0);
        psp_write32(rb + 0x28u, 0);
    }
    psp_ret(0);
}

static void hle_mpeg_rb_destruct(void) {
    g_mpeg.rb_addr = 0;
    psp_ret(0);
}

static uint32_t g_mpeg_avail_polls = 0;

static void hle_mpeg_rb_avail(void) {
    int32_t cap = g_mpeg.rb_packets > 0 ? g_mpeg.rb_packets : 512;
    size_t unconsumed = g_mpeg.ps_len + g_mpeg.v_es.queued_bytes + g_mpeg.a_es.queued_bytes;
    int32_t used_pkts = (int32_t)((unconsumed + 2047u) / 2048u);
    if (used_pkts == 0 && (g_mpeg.v_es.q_count > 0 || g_mpeg.a_es.q_count > 0))
        used_pkts = 1;
    int32_t avail = cap - used_pkts;
    if (avail < 0) avail = 0;
    if (g_mpeg.v_es.q_count >= MPEG_AU_CAP - 32 || g_mpeg.a_es.q_count >= MPEG_AU_CAP - 32)
        avail = 0;
    if (avail > 0 && !g_mpeg.eof && ++g_mpeg_avail_polls >= 4u)
        psp_threadman_yield();
    psp_ret((uint32_t)avail);
}

static void hle_mpeg_rb_put(void) {
    g_mpeg_avail_polls = 0;
    int32_t num_pkts = (int32_t)psp_arg(1);
    int32_t avail = (int32_t)psp_arg(2);
    int32_t req = num_pkts < avail ? num_pkts : avail;
    if (req <= 0 || !g_mpeg.rb_cb_addr || !g_mpeg.rb_data_addr) {
        psp_ret(0);
        return;
    }
    if (req > g_mpeg.rb_packets && g_mpeg.rb_packets > 0) req = g_mpeg.rb_packets;

    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_A0] = g_mpeg.rb_data_addr;
    psp_cpu.r[PSP_REG_A1] = (uint32_t)req;
    psp_cpu.r[PSP_REG_A2] = g_mpeg.rb_cb_arg;
    psp_call_from_host(g_mpeg.rb_cb_addr);
    int32_t got = (int32_t)psp_cpu.r[PSP_REG_V0];
    psp_cpu.r[PSP_REG_GP] = caller_gp;

    if (got > 0) {
        size_t bytes = (size_t)got * 2048u;
        if (g_mpeg.ps_len + bytes > g_mpeg.ps_cap) {
            size_t ncap = g_mpeg.ps_cap ? g_mpeg.ps_cap * 2u : 262144u;
            while (ncap < g_mpeg.ps_len + bytes) ncap *= 2u;
            uint8_t *nb = (uint8_t *)realloc(g_mpeg.ps_buf, ncap);
            if (nb) { g_mpeg.ps_buf = nb; g_mpeg.ps_cap = ncap; }
        }
        if (g_mpeg.ps_buf && g_mpeg.ps_len + bytes <= g_mpeg.ps_cap) {
            psp_mem_read_block(g_mpeg.ps_buf + g_mpeg.ps_len, g_mpeg.rb_data_addr, (uint32_t)bytes);
            g_mpeg.ps_len += bytes;
            g_mpeg.stream_fed += (uint32_t)bytes;
        }
        if (g_mpeg.stream_size > 0 && g_mpeg.stream_fed >= g_mpeg.stream_size)
            g_mpeg.eof = 1;
        mpeg_demux_step();
    } else if (got == 0) {
        g_mpeg.eof = 1;
        mpeg_demux_step();
    }
    psp_ret((uint32_t)got);
}

static void hle_mpeg_create(void) {
    uint32_t mpeg_ptr = psp_arg(0);
    uint32_t data_addr = psp_arg(1);
    if (mpeg_ptr) psp_write32(mpeg_ptr, data_addr ? data_addr : 1u);
    g_mpeg.active = 1;
    g_mpeg.eof = 0;
    g_mpeg.stream_fed = 0;
    psp_ret(0);
}

static void hle_mpeg_delete(void) {
    mpeg_free_codecs();
    es_clear(&g_mpeg.v_es, 1);
    es_clear(&g_mpeg.a_es, 1);
    g_mpeg.ps_len = 0;
    g_mpeg.active = 0;
    psp_ret(0);
}

static void hle_mpeg_query_stream_offset(void) {
    uint32_t buf = psp_arg(1);
    uint32_t off_out = psp_arg(2);
    uint32_t off = buf ? read_be32_guest(buf + 8u) : 0x800u;
    if (buf) g_mpeg.stream_size = read_be32_guest(buf + 12u);
    if (off_out) psp_write32(off_out, off);
    psp_ret(0);
}

static void hle_mpeg_query_stream_size(void) {
    uint32_t buf = psp_arg(0);
    uint32_t sz_out = psp_arg(1);
    uint32_t sz = buf ? read_be32_guest(buf + 12u) : 0u;
    g_mpeg.stream_size = sz;
    if (sz_out) psp_write32(sz_out, sz);
    psp_ret(0);
}

static void hle_mpeg_regist_stream(void) {
    uint32_t stype = psp_arg(1);
    psp_ret(stype == 0u ? 1u : 2u);
}

static void hle_mpeg_unregist_stream(void) { psp_ret(0); }

static void hle_mpeg_flush_all(void) {
    es_clear(&g_mpeg.v_es, 1);
    es_clear(&g_mpeg.a_es, 1);
    g_mpeg.ps_len = 0;
    g_mpeg.eof = 0;
    if (g_mpeg.v_ctx) avcodec_flush_buffers(g_mpeg.v_ctx);
    if (g_mpeg.a_ctx) avcodec_flush_buffers(g_mpeg.a_ctx);
    psp_ret(0);
}

static void hle_mpeg_malloc_avc_es(void) { psp_ret(1u); }
static void hle_mpeg_free_avc_es(void)   { psp_ret(0); }

static void hle_mpeg_query_atrac_es_size(void) {
    uint32_t es_sz = psp_arg(1), out_sz = psp_arg(2);
    if (es_sz)  psp_write32(es_sz, 0x800u);
    if (out_sz) psp_write32(out_sz, 0x2000u);
    psp_ret(0);
}

static void hle_mpeg_init_au(void) {
    uint32_t es_buf = psp_arg(1), au = psp_arg(2);
    if (au) {
        for (uint32_t i = 0; i < 24u; i += 4u) psp_write32(au + i, 0);
        psp_write32(au + 0x00u, 0xFFFFFFFFu);
        psp_write32(au + 0x04u, 0xFFFFFFFFu);
        psp_write32(au + 0x10u, es_buf);
    }
    psp_ret(0);
}

static void hle_mpeg_get_avc_au(void) {
    uint32_t au = psp_arg(2), attr = psp_arg(3);
    mpeg_demux_step();
    uint32_t hi = 0, lo = 0;
    if (!es_pop_au(&g_mpeg.v_es, &g_mpeg.cur_v_data, &g_mpeg.cur_v_size, &hi, &lo)) {
        psp_ret(SCE_MPEG_ERROR_NO_DATA);
        return;
    }
    if (au) {
        psp_write32(au + 0x00u, hi);
        psp_write32(au + 0x04u, lo);
        psp_write32(au + 0x14u, g_mpeg.cur_v_size);
    }
    if (attr) psp_write32(attr, 1u);
    psp_ret(0);
}

static void write_rgba_to_guest(uint32_t dst_addr, uint32_t frame_w, AVFrame *src) {
    if (!dst_addr || !src || src->width <= 0 || src->height <= 0) return;
    if (!g_mpeg.sws || g_mpeg.sws_w != src->width || g_mpeg.sws_h != src->height) {
        if (g_mpeg.sws) sws_freeContext(g_mpeg.sws);
        g_mpeg.sws = sws_getContext(src->width, src->height, (enum AVPixelFormat)src->format,
                                    src->width, src->height, AV_PIX_FMT_RGBA,
                                    SWS_BILINEAR, NULL, NULL, NULL);
        if (!g_mpeg.rgba_frame) g_mpeg.rgba_frame = av_frame_alloc();
        av_frame_unref(g_mpeg.rgba_frame);
        g_mpeg.rgba_frame->format = AV_PIX_FMT_RGBA;
        g_mpeg.rgba_frame->width = src->width;
        g_mpeg.rgba_frame->height = src->height;
        av_frame_get_buffer(g_mpeg.rgba_frame, 32);
        g_mpeg.sws_w = src->width;
        g_mpeg.sws_h = src->height;
    }
    if (!g_mpeg.sws || !g_mpeg.rgba_frame) return;

    sws_scale(g_mpeg.sws, (const uint8_t *const *)src->data, src->linesize,
              0, src->height, g_mpeg.rgba_frame->data, g_mpeg.rgba_frame->linesize);

    uint32_t row_bytes = (uint32_t)src->width * 4u;
    uint32_t stride_bytes = (frame_w > 0 ? frame_w : (uint32_t)src->width) * 4u;
    const uint8_t *sdata = g_mpeg.rgba_frame->data[0];
    int sstride = g_mpeg.rgba_frame->linesize[0];
    uint8_t *direct = (uint8_t *)psp_mem_unwatched_ptr(dst_addr, stride_bytes * (uint32_t)src->height);
    if (direct) {
        for (int y = 0; y < src->height; y++)
            memcpy(direct + (size_t)y * stride_bytes, sdata + (size_t)y * (size_t)sstride, row_bytes);
    } else {
        for (int y = 0; y < src->height; y++) {
            psp_mem_write_block(dst_addr + (uint32_t)y * stride_bytes,
                                sdata + (size_t)y * (size_t)sstride,
                                row_bytes);
        }
    }
    seto_ge_invalidate_vram(dst_addr, stride_bytes * (uint32_t)src->height);
}

static int ensure_video_decoder(void) {
    if (g_mpeg.v_ctx) return 1;
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) return 0;
    g_mpeg.v_ctx = avcodec_alloc_context3(c);
    g_mpeg.v_frame = av_frame_alloc();
    if (g_mpeg.v_ctx) {
        g_mpeg.v_ctx->thread_count = 2;
        g_mpeg.v_ctx->thread_type = FF_THREAD_SLICE;
    }
    if (!g_mpeg.v_ctx || !g_mpeg.v_frame || avcodec_open2(g_mpeg.v_ctx, c, NULL) < 0) {
        mpeg_free_codecs();
        return 0;
    }
    return 1;
}

static void hle_mpeg_avc_decode(void) {
    uint32_t frame_w = psp_arg(2);
    uint32_t buf_ptr_addr = psp_arg(3);
    uint32_t init_ptr = psp_arg(4);
    uint32_t dst_addr = buf_ptr_addr ? psp_read32(buf_ptr_addr) : 0u;
    int got_frame = 0;

    if (g_mpeg.cur_v_data && g_mpeg.cur_v_size > 0 && ensure_video_decoder()) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt) {
            pkt->data = g_mpeg.cur_v_data;
            pkt->size = (int)g_mpeg.cur_v_size;
            if (avcodec_send_packet(g_mpeg.v_ctx, pkt) >= 0 &&
                avcodec_receive_frame(g_mpeg.v_ctx, g_mpeg.v_frame) == 0) {
                write_rgba_to_guest(dst_addr, frame_w, g_mpeg.v_frame);
                got_frame = 1;
            }
            av_packet_free(&pkt);
        }
    }
    if (init_ptr) psp_write32(init_ptr, (uint32_t)got_frame);
    psp_ret(0);
}

static void hle_mpeg_avc_decode_stop(void) {
    uint32_t frame_w = psp_arg(1);
    uint32_t buf_ptr_addr = psp_arg(2);
    uint32_t init_ptr = psp_arg(3);
    uint32_t dst_addr = buf_ptr_addr ? psp_read32(buf_ptr_addr) : 0u;
    int got_frame = 0;

    if (g_mpeg.v_ctx && g_mpeg.v_frame) {
        avcodec_send_packet(g_mpeg.v_ctx, NULL);
        if (avcodec_receive_frame(g_mpeg.v_ctx, g_mpeg.v_frame) == 0) {
            write_rgba_to_guest(dst_addr, frame_w, g_mpeg.v_frame);
            got_frame = 1;
        }
    }
    if (init_ptr) psp_write32(init_ptr, (uint32_t)got_frame);
    psp_ret(0);
}

static void hle_mpeg_avc_flush(void) {
    if (g_mpeg.v_ctx) avcodec_flush_buffers(g_mpeg.v_ctx);
    psp_ret(0);
}

static void hle_mpeg_get_atrac_au(void) {
    uint32_t au = psp_arg(2), attr = psp_arg(3);
    mpeg_demux_step();
    uint32_t hi = 0, lo = 0;
    if (!es_pop_au(&g_mpeg.a_es, &g_mpeg.cur_a_data, &g_mpeg.cur_a_size, &hi, &lo)) {
        psp_ret(SCE_MPEG_ERROR_NO_DATA);
        return;
    }
    if (au) {
        psp_write32(au + 0x00u, hi);
        psp_write32(au + 0x04u, lo);
        psp_write32(au + 0x14u, g_mpeg.cur_a_size);
    }
    if (attr) psp_write32(attr, 0);
    psp_ret(0);
}

static int ensure_atrac3p_decoder(int block_align) {
    if (g_mpeg.a_ctx && g_mpeg.a_block_align == block_align) return 1;
    if (g_mpeg.swr) { swr_free(&g_mpeg.swr); g_mpeg.swr = NULL; }
    if (g_mpeg.a_ctx) avcodec_free_context(&g_mpeg.a_ctx);

    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    if (!c) return 0;
    g_mpeg.a_ctx = avcodec_alloc_context3(c);
    if (!g_mpeg.a_frame) g_mpeg.a_frame = av_frame_alloc();
    if (!g_mpeg.a_ctx || !g_mpeg.a_frame) return 0;

    av_channel_layout_default(&g_mpeg.a_ctx->ch_layout, 2);
    g_mpeg.a_ctx->sample_rate = 44100;
    g_mpeg.a_ctx->block_align = block_align;
    if (avcodec_open2(g_mpeg.a_ctx, c, NULL) < 0) {
        avcodec_free_context(&g_mpeg.a_ctx);
        return 0;
    }
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);
    swr_alloc_set_opts2(&g_mpeg.swr, &out_layout, AV_SAMPLE_FMT_S16, 44100,
                        &g_mpeg.a_ctx->ch_layout, g_mpeg.a_ctx->sample_fmt, 44100, 0, NULL);
    av_channel_layout_uninit(&out_layout);
    if (!g_mpeg.swr || swr_init(g_mpeg.swr) < 0) {
        if (g_mpeg.swr) { swr_free(&g_mpeg.swr); g_mpeg.swr = NULL; }
        avcodec_free_context(&g_mpeg.a_ctx);
        return 0;
    }
    g_mpeg.a_block_align = block_align;
    return 1;
}

static void hle_mpeg_atrac_decode(void) {
    uint32_t out_buf = psp_arg(2);
    int wrote = 0;

    if (out_buf && g_mpeg.cur_a_data && g_mpeg.cur_a_size > 0 &&
        ensure_atrac3p_decoder((int)g_mpeg.cur_a_size)) {
        AVPacket *pkt = av_packet_alloc();
        if (pkt) {
            pkt->data = g_mpeg.cur_a_data;
            pkt->size = (int)g_mpeg.cur_a_size;
            if (avcodec_send_packet(g_mpeg.a_ctx, pkt) >= 0 &&
                avcodec_receive_frame(g_mpeg.a_ctx, g_mpeg.a_frame) == 0) {
                int16_t pcm[2048 * 2];
                uint8_t *dst_planes[1] = { (uint8_t *)pcm };
                int conv = swr_convert(g_mpeg.swr, dst_planes, 2048,
                                       (const uint8_t **)g_mpeg.a_frame->extended_data,
                                       g_mpeg.a_frame->nb_samples);
                if (conv > 0) {
                    uint32_t cbytes = (uint32_t)conv * 4u;
                    psp_mem_write_block(out_buf, pcm, cbytes);
                    for (uint32_t i = cbytes; i < 8192u; i++) psp_write8(out_buf + i, 0);
                    wrote = 1;
                }
            }
            av_packet_free(&pkt);
        }
    }
    if (out_buf && !wrote) {
        for (uint32_t i = 0; i < 8192u; i += 4u) psp_write32(out_buf + i, 0);
    }
    psp_ret(0);
}

/* ============================================================================
 * 4. Remaining Standalone Non-Server NIDs
 * ============================================================================ */

static void hle_display_get_fps(void) {
    const float fps = 60000.0f / 1001.0f;
    psp_cpu.f[0] = fps;
    psp_ret(psp_f32_to_bits(fps));
}

static void hle_open_psid_get(void) {
    uint32_t dst = psp_arg(0);
    if (!dst) { psp_ret(0x80000103u); return; }
    static const uint8_t psid[16] = {
        0x53, 0x45, 0x54, 0x4F, 0x2D, 0x50, 0x53, 0x50,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
    };
    psp_mem_write_block(dst, psid, sizeof psid);
    psp_ret(0);
}

static void hle_load_exec(void) {
    psp_hle_call(0x05572A5Fu); /* sceKernelExitGame -> sets psp_exit_requested() */
}

static void hle_mod_stop_unload_self(void) {
    psp_ret(0);
}

static void hle_usb_module_ok(void) {
    psp_ret(0);
}

static void hle_adhoc_ptp_closed(void) {
    psp_ret(0x80410718u);
}

void seto_multimedia_register(void) {
    if (!g_aud_cs_init) {
        seto_mutex_init(&g_aud_cs);
        g_aud_cs_init = 1;
    }

    static const struct {
        uint32_t nid;
        const char *lib;
        const char *name;
        psp_hle_fn fn;
    } tbl[] = {
        /* sceAudio with live WinMM output */
        { 0x5EC81C55u, "sceAudio", "sceAudioChReserve",             hle_audio_ch_reserve },
        { 0x6FC46853u, "sceAudio", "sceAudioChRelease",             hle_audio_ch_release },
        { 0xCB2E439Eu, "sceAudio", "sceAudioSetChannelDataLen",     hle_audio_set_len },
        { 0xB7E1D8E7u, "sceAudio", "sceAudioChangeChannelVolume",   hle_audio_change_vol },
        { 0xE2D56B2Du, "sceAudio", "sceAudioOutputPanned",          hle_audio_output_panned },
        { 0x136CAF51u, "sceAudio", "sceAudioOutputBlocking",        hle_audio_blocking_unpanned },
        { 0x13F592BCu, "sceAudio", "sceAudioOutputPannedBlocking",  hle_audio_blocking_panned },

        /* sceAtrac3plus with FFmpeg ATRAC3/ATRAC3+ decoding */
        { 0x132F1ECAu, "sceAtrac3plus", "sceAtracReinit",             hle_atrac_reinit },
        { 0x7A20E7AFu, "sceAtrac3plus", "sceAtracSetDataAndGetID",    hle_atrac_set_data_and_get_id },
        { 0x61EB33F5u, "sceAtrac3plus", "sceAtracReleaseAtracID",     hle_atrac_release },
        { 0xD6A5F2F7u, "sceAtrac3plus", "sceAtracGetMaxSample",       hle_atrac_get_max_sample },
        { 0xA2BBA8BEu, "sceAtrac3plus", "sceAtracGetSoundSample",     hle_atrac_get_sound_sample },
        { 0x868120B5u, "sceAtrac3plus", "sceAtracSetLoopNum",         hle_atrac_set_loop_num },
        { 0x644E5607u, "sceAtrac3plus", "sceAtracResetPlayPosition",  hle_atrac_reset_pos },
        { 0xFAA4F89Bu, "sceAtrac3plus", "sceAtracGetLoopStatus",      hle_atrac_get_loop_status },
        { 0x6A8C3CD5u, "sceAtrac3plus", "sceAtracDecodeData",         hle_atrac_decode_data },

        /* sceMpeg (all 25 NIDs) */
        { 0x682A619Bu, "sceMpeg", "sceMpegInit",                      hle_mpeg_init },
        { 0x874624D6u, "sceMpeg", "sceMpegFinish",                    hle_mpeg_finish },
        { 0xC132E22Fu, "sceMpeg", "sceMpegQueryMemSize",              hle_mpeg_query_mem_size },
        { 0xD8C5F121u, "sceMpeg", "sceMpegCreate",                    hle_mpeg_create },
        { 0x606A4649u, "sceMpeg", "sceMpegDelete",                    hle_mpeg_delete },
        { 0xD7A29F46u, "sceMpeg", "sceMpegRingbufferQueryMemSize",    hle_mpeg_rb_query_mem_size },
        { 0x37295ED8u, "sceMpeg", "sceMpegRingbufferConstruct",       hle_mpeg_rb_construct },
        { 0x13407F13u, "sceMpeg", "sceMpegRingbufferDestruct",        hle_mpeg_rb_destruct },
        { 0xB5F6DC87u, "sceMpeg", "sceMpegRingbufferAvailableSize",   hle_mpeg_rb_avail },
        { 0xB240A59Eu, "sceMpeg", "sceMpegRingbufferPut",             hle_mpeg_rb_put },
        { 0x21FF80E4u, "sceMpeg", "sceMpegQueryStreamOffset",         hle_mpeg_query_stream_offset },
        { 0x611E9E11u, "sceMpeg", "sceMpegQueryStreamSize",           hle_mpeg_query_stream_size },
        { 0x42560F23u, "sceMpeg", "sceMpegRegistStream",              hle_mpeg_regist_stream },
        { 0x591A4AA2u, "sceMpeg", "sceMpegUnRegistStream",            hle_mpeg_unregist_stream },
        { 0x707B7629u, "sceMpeg", "sceMpegFlushAllStream",            hle_mpeg_flush_all },
        { 0xA780CF7Eu, "sceMpeg", "sceMpegMallocAvcEsBuf",            hle_mpeg_malloc_avc_es },
        { 0xCEB870B1u, "sceMpeg", "sceMpegFreeAvcEsBuf",              hle_mpeg_free_avc_es },
        { 0xF8DCB679u, "sceMpeg", "sceMpegQueryAtracEsSize",          hle_mpeg_query_atrac_es_size },
        { 0x167AFD9Eu, "sceMpeg", "sceMpegInitAu",                    hle_mpeg_init_au },
        { 0xFE246728u, "sceMpeg", "sceMpegGetAvcAu",                  hle_mpeg_get_avc_au },
        { 0x0E3C2E9Du, "sceMpeg", "sceMpegAvcDecode",                 hle_mpeg_avc_decode },
        { 0x4571CC64u, "sceMpeg", "sceMpegAvcDecodeFlush",            hle_mpeg_avc_flush },
        { 0x740FCCD1u, "sceMpeg", "sceMpegAvcDecodeStop",             hle_mpeg_avc_decode_stop },
        { 0xE1CE83A7u, "sceMpeg", "sceMpegGetAtracAu",                hle_mpeg_get_atrac_au },
        { 0x800C44DFu, "sceMpeg", "sceMpegAtracDecode",               hle_mpeg_atrac_decode },

        /* Remaining standalone non-server NIDs */
        { 0xDBA6C4C4u, "sceDisplay",       "sceDisplayGetFramePerSec",  hle_display_get_fps },
        { 0xC69BEBCEu, "sceOpenPSID",      "sceOpenPSIDGetOpenPSID",    hle_open_psid_get },
        { 0xBD2F1094u, "LoadExecForUser",  "sceKernelLoadExec",         hle_load_exec },
        { 0x0D5BC6D2u, "sceUtility",       "sceUtilityLoadUsbModule",   hle_usb_module_ok },
        { 0xF64910F0u, "sceUtility",       "sceUtilityUnloadUsbModule", hle_usb_module_ok },
        { 0xFC6FC07Bu, "sceNetAdhoc",      "sceNetAdhocPtpConnect",     hle_adhoc_ptp_closed },
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        psp_hle_register(tbl[i].nid, tbl[i].lib, tbl[i].name, tbl[i].fn);

    psp_hle_register_unnamed(0x8F2DF740u, "ModuleMgrForUser", hle_mod_stop_unload_self);
}

void seto_multimedia_shutdown(void) {
    if (g_aud_cs_init) {
        seto_mutex_lock(&g_aud_cs);
        for (int i = 0; i < AUDIO_CHANNELS; i++) {
            audio_close_ch(&g_aud[i]);
            g_aud[i].reserved = 0;
        }
        seto_mutex_unlock(&g_aud_cs);
    }
#ifndef _WIN32
    if (g_sdl_aud_init) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_sdl_aud_init = 0;
    }
#endif
    atrac_reset_all();
    mpeg_reset_state();
}
