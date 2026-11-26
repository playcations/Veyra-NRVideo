/*
 * test_nvdec.c - Standalone Windows test harness verifying avcodec-63.dll NVDEC proxy
 * decodes real 1080p Annex-B H.265 frames on NVIDIA NVDEC inside Proton 11.0.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

typedef const AVCodec *(*fn_find_decoder)(enum AVCodecID);
typedef AVCodecContext *(*fn_alloc_ctx)(const AVCodec *);
typedef int (*fn_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
typedef int (*fn_send_pkt)(AVCodecContext *, const AVPacket *);
typedef int (*fn_recv_frame)(AVCodecContext *, AVFrame *);
typedef void (*fn_flush)(AVCodecContext *);
typedef void (*fn_free_ctx)(AVCodecContext **);
typedef AVPacket *(*fn_pkt_alloc)(void);
typedef int (*fn_new_pkt)(AVPacket *, int);
typedef void (*fn_pkt_free)(AVPacket **);
typedef AVFrame *(*fn_frame_alloc)(void);
typedef void (*fn_frame_free)(AVFrame **);

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_nvdec.exe <annexb.h265> [h264|hevc]\n");
        return 2;
    }
    int is_h264 = (argc >= 3 && strcmp(argv[2], "h264") == 0);

    HMODULE avc = LoadLibraryA("avcodec-63.dll");
    HMODULE avu = LoadLibraryA("avutil-61.dll");
    if (!avc || !avu) {
        fprintf(stderr, "Failed to load avcodec-63.dll (%lu) or avutil-61.dll (%lu)\n",
                (unsigned long)GetLastError(), (unsigned long)GetLastError());
        return 1;
    }

    fn_find_decoder p_find = (fn_find_decoder)GetProcAddress(avc, "avcodec_find_decoder");
    fn_alloc_ctx    p_alloc = (fn_alloc_ctx)GetProcAddress(avc, "avcodec_alloc_context3");
    fn_open2        p_open  = (fn_open2)GetProcAddress(avc, "avcodec_open2");
    fn_send_pkt     p_send  = (fn_send_pkt)GetProcAddress(avc, "avcodec_send_packet");
    fn_recv_frame   p_recv  = (fn_recv_frame)GetProcAddress(avc, "avcodec_receive_frame");
    fn_flush        p_flush = (fn_flush)GetProcAddress(avc, "avcodec_flush_buffers");
    fn_free_ctx     p_free  = (fn_free_ctx)GetProcAddress(avc, "avcodec_free_context");
    fn_pkt_alloc    p_palloc = (fn_pkt_alloc)GetProcAddress(avc, "av_packet_alloc");
    fn_new_pkt      p_pnew   = (fn_new_pkt)GetProcAddress(avc, "av_new_packet");
    fn_pkt_free     p_pfree  = (fn_pkt_free)GetProcAddress(avc, "av_packet_free");
    fn_frame_alloc  p_falloc = (fn_frame_alloc)GetProcAddress(avu, "av_frame_alloc");
    fn_frame_free   p_ffree  = (fn_frame_free)GetProcAddress(avu, "av_frame_free");

    /* Read length-prefixed frame stream: uint32_t len, bytes[len], ... */
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", argv[1]); return 1; }

    const AVCodec *codec = p_find(is_h264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    AVCodecContext *ctx = p_alloc(codec);
    ctx->width = 1920;
    ctx->height = 1080;
    ctx->thread_count = 1;
    if (p_open(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 failed\n");
        return 1;
    }

    AVFrame *frame = p_falloc();
    uint32_t len = 0;
    int64_t idx = 0;
    int decoded = 0;
    double total_ms = 0.0, max_ms = 0.0;

    while (fread(&len, 4, 1, fp) == 1 && len > 0 && len < 8 * 1024 * 1024) {
        AVPacket *pkt = p_palloc();
        p_pnew(pkt, (int)len);
        if (fread(pkt->data, 1, len, fp) != len) { p_pfree(&pkt); break; }
        pkt->pts = idx;
        pkt->dts = AV_NOPTS_VALUE;

        double t0 = now_ms();
        int sr = p_send(ctx, pkt);
        if (sr < 0) {
            fprintf(stderr, "send_packet failed at frame %lld: %d\n", (long long)idx, sr);
            p_pfree(&pkt);
            return 1;
        }
        int got = 0;
        while (p_recv(ctx, frame) == 0) {
            double dt = now_ms() - t0;
            if (decoded > 0) { /* skip 1st frame init */
                total_ms += dt;
                if (dt > max_ms) max_ms = dt;
            }
            if (decoded < 3 || decoded == 29) {
                printf("frame #%d: %dx%d fmt=%d pts=%lld (sent=%lld) y0=%u uv0=%u dt=%.2fms\n",
                       decoded, frame->width, frame->height, frame->format,
                       (long long)frame->pts, (long long)idx,
                       frame->data[0] ? frame->data[0][0] : 0,
                       frame->data[1] ? frame->data[1][0] : 0, dt);
            }
            decoded++;
            got++;
        }
        if (got == 0 && idx < 5) {
            printf("pkt #%lld: buffered (0 frames emitted yet)\n", (long long)idx);
        }
        p_pfree(&pkt);
        idx++;
    }

    p_flush(ctx);
    p_ffree(&frame);
    p_free(&ctx);
    fclose(fp);

    printf("RESULT: sent=%lld decoded=%d avg_decode_ms=%.2f max_decode_ms=%.2f\n",
           (long long)idx, decoded, decoded > 1 ? total_ms / (decoded - 1) : 0.0, max_ms);
    return (decoded >= idx - 1 && decoded > 0) ? 0 : 1;
}
