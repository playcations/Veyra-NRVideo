/*
 * avcodec_nvdec_proxy.c - Drop-in NVDEC GPU hardware decode proxy for Veyra 2.2.x (FFmpeg 9.0.1 / avcodec-63.dll)
 *
 * Forwards all untouched exports to avcodec-63-orig.dll via PE export forwarding.
 * Intercepts Annex-B H.265/HEVC and H.264 software decode sessions (e.g. PS5 Remote Play
 * under Proton/Wine where D3D12VA is unavailable) and decodes them on NVIDIA NVDEC via
 * nvcuda.dll + nvcuvid.dll (SveSop/nvidia-libs).
 *
 * Output frames are standard AV_PIX_FMT_NV12 (8-bit) or AV_PIX_FMT_P010LE (10-bit HDR)
 * allocated via Veyra's own avutil-61.dll, preserving exact PTS and color metadata.
 *
 * Env toggles:
 *   VEYRA_NVDEC=0      Disable NVDEC proxy (fall back to avcodec-63-orig.dll)
 *   VEYRA_NVDEC_LOG=1  Verbose stderr/log diagnostics
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include <ffnvcodec/dynlink_loader.h>

/* ---- Dynamic handles to original FFmpeg DLLs ---- */
typedef const AVCodec *(*fn_avcodec_find_decoder)(enum AVCodecID id);
typedef int (*fn_avcodec_open2)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
typedef int (*fn_avcodec_send_packet)(AVCodecContext *avctx, const AVPacket *avpkt);
typedef int (*fn_avcodec_receive_frame)(AVCodecContext *avctx, AVFrame *frame);
typedef void (*fn_avcodec_flush_buffers)(AVCodecContext *avctx);
typedef void (*fn_avcodec_free_context)(AVCodecContext **avctx);

typedef void (*fn_av_frame_unref)(AVFrame *frame);
typedef int (*fn_av_frame_get_buffer)(AVFrame *frame, int align);
typedef void (*fn_av_log)(void *avcl, int level, const char *fmt, ...);

static HMODULE g_orig_avcodec = NULL;
static HMODULE g_avutil = NULL;
static fn_avcodec_find_decoder p_orig_find_decoder = NULL;
static fn_avcodec_open2 p_orig_open2 = NULL;
static fn_avcodec_send_packet p_orig_send_packet = NULL;
static fn_avcodec_receive_frame p_orig_receive_frame = NULL;
static fn_avcodec_flush_buffers p_orig_flush_buffers = NULL;
static fn_avcodec_free_context p_orig_free_context = NULL;
static fn_av_frame_unref p_av_frame_unref = NULL;
static fn_av_frame_get_buffer p_av_frame_get_buffer = NULL;
static fn_av_log p_av_log = NULL;

/* ---- Global CUDA/NVDEC state ---- */
static CRITICAL_SECTION g_lock;
static int g_lock_init = 0;
static int g_cuda_attempted = 0;
static int g_cuda_ready = 0;
static CudaFunctions *g_cu = NULL;
static CuvidFunctions *g_cv = NULL;
static CUcontext g_cu_ctx = NULL;
static CUvideoctxlock g_ctx_lock = NULL;

static int nvdec_verbose(void) {
    const char *v = getenv("VEYRA_NVDEC_LOG");
    return v && v[0] == '1';
}

static void nvdec_debug_file(const char *msg) {
    if (!nvdec_verbose()) return;
    FILE *f = fopen("Z:\\tmp\\veyra-nvdec-proxy.log", "a");
    if (f) {
        fprintf(f, "[pid=%lu] %s\n", (unsigned long)GetCurrentProcessId(), msg);
        fclose(f);
    }
}

static void nvdec_log(AVCodecContext *avctx, int level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (p_av_log) {
        p_av_log(avctx, level, "[nvdec-proxy] %s\n", buf);
    }
    nvdec_debug_file(buf);
    if (nvdec_verbose() || level <= AV_LOG_WARNING) {
        fprintf(stderr, "[nvdec-proxy] %s\n", buf);
        fflush(stderr);
    }
}

static int ensure_orig_loaded(void) {
    if (p_orig_open2 && p_av_frame_get_buffer)
        return 1;
    EnterCriticalSection(&g_lock);
    if (!g_orig_avcodec) {
        /* Load avcodec-63-orig.dll from the same directory as avcodec-63.dll */
        char mod_path[MAX_PATH] = {0};
        HMODULE self = NULL;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&ensure_orig_loaded, &self);
        if (self && GetModuleFileNameA(self, mod_path, sizeof(mod_path))) {
            char *slash = strrchr(mod_path, '\\');
            if (slash) {
                snprintf(slash + 1, sizeof(mod_path) - (size_t)(slash + 1 - mod_path), "avcodec-63-orig.dll");
                g_orig_avcodec = LoadLibraryA(mod_path);
            }
        }
        if (!g_orig_avcodec)
            g_orig_avcodec = LoadLibraryA("avcodec-63-orig.dll");
    }
    if (!g_avutil) {
        g_avutil = GetModuleHandleA("avutil-61.dll");
        if (!g_avutil)
            g_avutil = LoadLibraryA("avutil-61.dll");
    }
    if (g_orig_avcodec && !p_orig_open2) {
        p_orig_find_decoder  = (fn_avcodec_find_decoder)GetProcAddress(g_orig_avcodec, "avcodec_find_decoder");
        p_orig_open2         = (fn_avcodec_open2)GetProcAddress(g_orig_avcodec, "avcodec_open2");
        p_orig_send_packet   = (fn_avcodec_send_packet)GetProcAddress(g_orig_avcodec, "avcodec_send_packet");
        p_orig_receive_frame = (fn_avcodec_receive_frame)GetProcAddress(g_orig_avcodec, "avcodec_receive_frame");
        p_orig_flush_buffers = (fn_avcodec_flush_buffers)GetProcAddress(g_orig_avcodec, "avcodec_flush_buffers");
        p_orig_free_context  = (fn_avcodec_free_context)GetProcAddress(g_orig_avcodec, "avcodec_free_context");
    }
    if (g_avutil && !p_av_frame_get_buffer) {
        p_av_frame_unref      = (fn_av_frame_unref)GetProcAddress(g_avutil, "av_frame_unref");
        p_av_frame_get_buffer = (fn_av_frame_get_buffer)GetProcAddress(g_avutil, "av_frame_get_buffer");
        p_av_log              = (fn_av_log)GetProcAddress(g_avutil, "av_log");
    }
    LeaveCriticalSection(&g_lock);
    return (p_orig_open2 != NULL && p_av_frame_get_buffer != NULL);
}

static int ensure_cuda_init(AVCodecContext *avctx) {
    EnterCriticalSection(&g_lock);
    if (g_cuda_attempted) {
        int r = g_cuda_ready;
        LeaveCriticalSection(&g_lock);
        return r;
    }
    g_cuda_attempted = 1;

    if (cuda_load_functions(&g_cu, NULL) < 0 || !g_cu) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuda_load_functions failed (nvcuda.dll unavailable)");
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    if (cuvid_load_functions(&g_cv, NULL) < 0 || !g_cv) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuvid_load_functions failed (nvcuvid.dll unavailable)");
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    CUresult cr = g_cu->cuInit(0);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuInit failed: %d", (int)cr);
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    CUdevice dev = 0;
    cr = g_cu->cuDeviceGet(&dev, 0);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuDeviceGet(0) failed: %d", (int)cr);
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    char dev_name[128] = {0};
    if (g_cu->cuDeviceGetName)
        g_cu->cuDeviceGetName(dev_name, sizeof(dev_name) - 1, dev);

    cr = g_cu->cuCtxCreate(&g_cu_ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuCtxCreate failed: %d", (int)cr);
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    CUcontext popped = NULL;
    g_cu->cuCtxPopCurrent(&popped);

    cr = g_cv->cuvidCtxLockCreate(&g_ctx_lock, g_cu_ctx);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_WARNING, "cuvidCtxLockCreate failed: %d", (int)cr);
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    g_cuda_ready = 1;
    nvdec_log(avctx, AV_LOG_INFO, "NVDEC initialized on GPU 0 (%s)", dev_name[0] ? dev_name : "NVIDIA");
    LeaveCriticalSection(&g_lock);
    return 1;
}

/* ---- Per-AVCodecContext NVDEC session ---- */
#define DISP_QUEUE_CAP 32
#define MAX_SESSIONS   16

typedef struct NvdecSession {
    AVCodecContext *avctx;
    cudaVideoCodec  cuda_codec;
    CUvideoparser   parser;
    CUvideodecoder  decoder;

    int coded_w;
    int coded_h;
    int disp_w;
    int disp_h;
    int target_w;
    int target_h;
    int bit_depth;
    int num_surfaces;
    enum AVPixelFormat pix_fmt;

    /* Video signal description from SPS/VUI */
    int has_color_info;
    enum AVColorRange color_range;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;

    /* Decoded display queue (FIFO) */
    CUVIDPARSERDISPINFO q[DISP_QUEUE_CAP];
    int q_head;
    int q_count;

    int internal_error;
    uint64_t frames_decoded;
} NvdecSession;

static NvdecSession *g_sessions[MAX_SESSIONS];

static NvdecSession *find_session(const AVCodecContext *avctx) {
    if (!avctx) return NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (g_sessions[i] && g_sessions[i]->avctx == avctx) {
            NvdecSession *s = g_sessions[i];
            LeaveCriticalSection(&g_lock);
            return s;
        }
    }
    LeaveCriticalSection(&g_lock);
    return NULL;
}

static int register_session(NvdecSession *s) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!g_sessions[i]) {
            g_sessions[i] = s;
            LeaveCriticalSection(&g_lock);
            return 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}

static NvdecSession *unregister_session(const AVCodecContext *avctx) {
    if (!avctx) return NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (g_sessions[i] && g_sessions[i]->avctx == avctx) {
            NvdecSession *s = g_sessions[i];
            g_sessions[i] = NULL;
            LeaveCriticalSection(&g_lock);
            return s;
        }
    }
    LeaveCriticalSection(&g_lock);
    return NULL;
}

/* ---- CUVID callbacks ---- */
static int CUDAAPI cb_video_sequence(void *opaque, CUVIDEOFORMAT *fmt) {
    NvdecSession *s = (NvdecSession *)opaque;
    if (!s || !fmt) return 0;

    int disp_w = fmt->display_area.right - fmt->display_area.left;
    int disp_h = fmt->display_area.bottom - fmt->display_area.top;
    if (disp_w <= 0) disp_w = (int)fmt->coded_width;
    if (disp_h <= 0) disp_h = (int)fmt->coded_height;

    int target_w = (disp_w + 1) & ~1;
    int target_h = (disp_h + 1) & ~1;
    int bit_depth = fmt->bit_depth_luma_minus8 + 8;
    int surfaces = (int)fmt->min_num_decode_surfaces + 4;
    if (surfaces < 8) surfaces = 8;
    if (surfaces > 20) surfaces = 20;

    /* Capture VUI color description */
    s->has_color_info = 1;
    s->color_range = fmt->video_signal_description.video_full_range_flag ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    s->color_primaries = (enum AVColorPrimaries)fmt->video_signal_description.color_primaries;
    s->color_trc = (enum AVColorTransferCharacteristic)fmt->video_signal_description.transfer_characteristics;
    s->colorspace = (enum AVColorSpace)fmt->video_signal_description.matrix_coefficients;

    if (s->decoder &&
        s->coded_w == (int)fmt->coded_width &&
        s->coded_h == (int)fmt->coded_height &&
        s->disp_w == disp_w &&
        s->disp_h == disp_h &&
        s->bit_depth == bit_depth) {
        return s->num_surfaces;
    }

    if (s->decoder) {
        g_cv->cuvidDestroyDecoder(s->decoder);
        s->decoder = NULL;
    }

    CUVIDDECODECREATEINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.CodecType = fmt->codec;
    ci.ChromaFormat = fmt->chroma_format;
    ci.OutputFormat = (bit_depth > 8) ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
    ci.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
    ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    ci.ulNumDecodeSurfaces = (unsigned long)surfaces;
    ci.ulNumOutputSurfaces = 2;
    ci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    ci.ulWidth = fmt->coded_width;
    ci.ulHeight = fmt->coded_height;
    ci.ulMaxWidth = fmt->coded_width > 1920 ? fmt->coded_width : 1920;
    ci.ulMaxHeight = fmt->coded_height > 1088 ? fmt->coded_height : 1088;
    ci.display_area.left = (short)fmt->display_area.left;
    ci.display_area.top = (short)fmt->display_area.top;
    ci.display_area.right = (short)fmt->display_area.right;
    ci.display_area.bottom = (short)fmt->display_area.bottom;
    ci.ulTargetWidth = (unsigned long)target_w;
    ci.ulTargetHeight = (unsigned long)target_h;
    ci.vidLock = g_ctx_lock;

    CUresult cr = g_cv->cuvidCreateDecoder(&s->decoder, &ci);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(s->avctx, AV_LOG_ERROR, "cuvidCreateDecoder failed: %d (%dx%d %dbit)",
                  (int)cr, disp_w, disp_h, bit_depth);
        s->internal_error = AVERROR(EINVAL);
        return 0;
    }

    s->coded_w = (int)fmt->coded_width;
    s->coded_h = (int)fmt->coded_height;
    s->disp_w = disp_w;
    s->disp_h = disp_h;
    s->target_w = target_w;
    s->target_h = target_h;
    s->bit_depth = bit_depth;
    s->num_surfaces = surfaces;
    s->pix_fmt = (bit_depth > 8) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;

    if (s->avctx) {
        s->avctx->width = target_w;
        s->avctx->height = target_h;
        s->avctx->pix_fmt = s->pix_fmt;
    }

    nvdec_log(s->avctx, AV_LOG_INFO, "NVDEC sequence active: %s %dx%d (coded %dx%d) %dbit -> %s surfaces=%d",
              fmt->codec == cudaVideoCodec_HEVC ? "HEVC" : "H264",
              target_w, target_h, s->coded_w, s->coded_h, bit_depth,
              s->pix_fmt == AV_PIX_FMT_P010LE ? "P010LE" : "NV12", surfaces);

    return surfaces;
}

static int CUDAAPI cb_decode_picture(void *opaque, CUVIDPICPARAMS *pic) {
    NvdecSession *s = (NvdecSession *)opaque;
    if (!s || !s->decoder) return 0;
    CUresult cr = g_cv->cuvidDecodePicture(s->decoder, pic);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(s->avctx, AV_LOG_ERROR, "cuvidDecodePicture failed: %d", (int)cr);
        s->internal_error = AVERROR_EXTERNAL;
        return 0;
    }
    return 1;
}

static int CUDAAPI cb_display_picture(void *opaque, CUVIDPARSERDISPINFO *disp) {
    NvdecSession *s = (NvdecSession *)opaque;
    if (!s || !disp) return 0;
    if (s->q_count >= DISP_QUEUE_CAP) {
        /* Drop oldest if consumer stalled */
        s->q_head = (s->q_head + 1) % DISP_QUEUE_CAP;
        s->q_count--;
    }
    int tail = (s->q_head + s->q_count) % DISP_QUEUE_CAP;
    s->q[tail] = *disp;
    s->q_count++;
    return 1;
}

static int create_parser(NvdecSession *s) {
    CUVIDPARSERPARAMS pp;
    memset(&pp, 0, sizeof(pp));
    pp.CodecType = s->cuda_codec;
    pp.ulMaxNumDecodeSurfaces = 1;
    pp.ulClockRate = 0;
    pp.ulErrorThreshold = 100;
    pp.ulMaxDisplayDelay = 0; /* 0-frame low-latency display order for Remote Play */
    pp.pUserData = s;
    pp.pfnSequenceCallback = cb_video_sequence;
    pp.pfnDecodePicture = cb_decode_picture;
    pp.pfnDisplayPicture = cb_display_picture;

    CUresult cr = g_cv->cuvidCreateVideoParser(&s->parser, &pp);
    if (cr != CUDA_SUCCESS) {
        nvdec_log(s->avctx, AV_LOG_ERROR, "cuvidCreateVideoParser failed: %d", (int)cr);
        return 0;
    }
    return 1;
}

static void destroy_session(NvdecSession *s) {
    if (!s) return;
    if (g_cuda_ready) {
        CUcontext dummy = NULL;
        g_cu->cuCtxPushCurrent(g_cu_ctx);
        if (s->parser)  g_cv->cuvidDestroyVideoParser(s->parser);
        if (s->decoder) g_cv->cuvidDestroyDecoder(s->decoder);
        g_cu->cuCtxPopCurrent(&dummy);
    }
    free(s);
}

/* ---- Intercepted FFmpeg exports ---- */
int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options) {
    if (!ensure_orig_loaded())
        return AVERROR(ENOSYS);

    const char *env_dis = getenv("VEYRA_NVDEC");
    int disabled = (env_dis && env_dis[0] == '0');

    const AVCodec *eff_codec = codec ? codec : (avctx ? avctx->codec : NULL);
    int want_nvdec = 0;
    cudaVideoCodec cu_codec = cudaVideoCodec_NumCodecs;

    if (!disabled && avctx && eff_codec && avctx->hw_device_ctx == NULL && avctx->extradata_size == 0) {
        const AVCodec *hevc_dec = p_orig_find_decoder ? p_orig_find_decoder(AV_CODEC_ID_HEVC) : NULL;
        const AVCodec *h264_dec = p_orig_find_decoder ? p_orig_find_decoder(AV_CODEC_ID_H264) : NULL;
        if (eff_codec == hevc_dec) {
            want_nvdec = 1;
            cu_codec = cudaVideoCodec_HEVC;
        } else if (eff_codec == h264_dec) {
            want_nvdec = 1;
            cu_codec = cudaVideoCodec_H264;
        }
    }

    /* Always open original skeleton so avctx internal fields remain valid.
     * Veyra hardcodes thread_count=1 for Remote Play to avoid FF_THREAD_FRAME
     * buffering delay; use FF_THREAD_SLICE with 4 threads on software fallback
     * for zero extra frame latency. */
    if (avctx && avctx->hw_device_ctx == NULL && avctx->extradata_size == 0 && avctx->thread_count == 1) {
        avctx->thread_type = FF_THREAD_SLICE;
        avctx->thread_count = 4;
    }
    int orig_ret = p_orig_open2(avctx, codec, options);
    if (orig_ret < 0 || !want_nvdec)
        return orig_ret;

    if (!ensure_cuda_init(avctx)) {
        nvdec_log(avctx, AV_LOG_WARNING, "CUDA/NVDEC unavailable; using original software decoder");
        return orig_ret;
    }

    NvdecSession *s = (NvdecSession *)calloc(1, sizeof(NvdecSession));
    if (!s) return orig_ret;
    s->avctx = avctx;
    s->cuda_codec = cu_codec;

    CUcontext dummy = NULL;
    g_cu->cuCtxPushCurrent(g_cu_ctx);
    int ok = create_parser(s);
    g_cu->cuCtxPopCurrent(&dummy);

    if (!ok || !register_session(s)) {
        destroy_session(s);
        return orig_ret;
    }

    nvdec_log(avctx, AV_LOG_INFO, "NVDEC GPU hardware decode attached for %s %dx%d",
              cu_codec == cudaVideoCodec_HEVC ? "HEVC" : "H264", avctx->width, avctx->height);
    return 0;
}

int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) {
    if (!ensure_orig_loaded())
        return AVERROR(ENOSYS);

    NvdecSession *s = find_session(avctx);
    if (!s)
        return p_orig_send_packet(avctx, avpkt);

    CUVIDSOURCEDATAPACKET cupkt;
    memset(&cupkt, 0, sizeof(cupkt));
    if (avpkt && avpkt->data && avpkt->size > 0) {
        cupkt.payload = avpkt->data;
        cupkt.payload_size = (unsigned long)avpkt->size;
        cupkt.flags = CUVID_PKT_ENDOFPICTURE;
        if (avpkt->pts != AV_NOPTS_VALUE) {
            cupkt.flags |= CUVID_PKT_TIMESTAMP;
            cupkt.timestamp = avpkt->pts;
        }
    } else {
        cupkt.flags = CUVID_PKT_ENDOFSTREAM;
    }

    s->internal_error = 0;
    CUcontext dummy = NULL;
    g_cu->cuCtxPushCurrent(g_cu_ctx);
    CUresult cr = g_cv->cuvidParseVideoData(s->parser, &cupkt);
    g_cu->cuCtxPopCurrent(&dummy);

    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_ERROR, "cuvidParseVideoData failed: %d", (int)cr);
        return AVERROR_INVALIDDATA;
    }
    if (s->internal_error < 0)
        return s->internal_error;
    return 0;
}

int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) {
    if (!ensure_orig_loaded())
        return AVERROR(ENOSYS);

    NvdecSession *s = find_session(avctx);
    if (!s)
        return p_orig_receive_frame(avctx, frame);

    if (s->q_count <= 0)
        return AVERROR(EAGAIN);
    if (!s->decoder || !frame)
        return AVERROR(EINVAL);

    CUVIDPARSERDISPINFO disp = s->q[s->q_head];
    s->q_head = (s->q_head + 1) % DISP_QUEUE_CAP;
    s->q_count--;

    p_av_frame_unref(frame);
    frame->format = s->pix_fmt;
    frame->width = s->target_w;
    frame->height = s->target_h;
    frame->pts = disp.timestamp;
    frame->pkt_dts = disp.timestamp;
    frame->chroma_location = AVCHROMA_LOC_LEFT;
    if (s->has_color_info) {
        frame->color_range = s->color_range;
        frame->color_primaries = s->color_primaries;
        frame->color_trc = s->color_trc;
        frame->colorspace = s->colorspace;
    }

    int ret = p_av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        nvdec_log(avctx, AV_LOG_ERROR, "av_frame_get_buffer failed: %d", ret);
        return ret;
    }

    CUVIDPROCPARAMS procp;
    memset(&procp, 0, sizeof(procp));
    procp.progressive_frame = disp.progressive_frame;
    procp.top_field_first = disp.top_field_first;

    CUdeviceptr mapped = 0;
    unsigned int pitch = 0;
    CUcontext dummy = NULL;
    g_cu->cuCtxPushCurrent(g_cu_ctx);

    CUresult cr = g_cv->cuvidMapVideoFrame(s->decoder, disp.picture_index, &mapped, &pitch, &procp);
    if (cr != CUDA_SUCCESS) {
        g_cu->cuCtxPopCurrent(&dummy);
        nvdec_log(avctx, AV_LOG_ERROR, "cuvidMapVideoFrame failed: %d", (int)cr);
        p_av_frame_unref(frame);
        return AVERROR_EXTERNAL;
    }

    size_t row_bytes = (size_t)s->target_w * (s->pix_fmt == AV_PIX_FMT_P010LE ? 2u : 1u);

    /* Plane 0: Y */
    CUDA_MEMCPY2D cpy_y;
    memset(&cpy_y, 0, sizeof(cpy_y));
    cpy_y.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy_y.srcDevice     = mapped;
    cpy_y.srcPitch      = pitch;
    cpy_y.dstMemoryType = CU_MEMORYTYPE_HOST;
    cpy_y.dstHost       = frame->data[0];
    cpy_y.dstPitch      = (size_t)frame->linesize[0];
    cpy_y.WidthInBytes  = row_bytes;
    cpy_y.Height        = (size_t)s->target_h;
    cr = g_cu->cuMemcpy2D(&cpy_y);

    /* Plane 1: interleaved UV */
    if (cr == CUDA_SUCCESS) {
        CUDA_MEMCPY2D cpy_uv;
        memset(&cpy_uv, 0, sizeof(cpy_uv));
        cpy_uv.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        cpy_uv.srcDevice     = mapped;
        cpy_uv.srcY          = (size_t)s->target_h;
        cpy_uv.srcPitch      = pitch;
        cpy_uv.dstMemoryType = CU_MEMORYTYPE_HOST;
        cpy_uv.dstHost       = frame->data[1];
        cpy_uv.dstPitch      = (size_t)frame->linesize[1];
        cpy_uv.WidthInBytes  = row_bytes;
        cpy_uv.Height        = (size_t)(s->target_h / 2);
        cr = g_cu->cuMemcpy2D(&cpy_uv);
    }

    g_cv->cuvidUnmapVideoFrame(s->decoder, mapped);
    g_cu->cuCtxPopCurrent(&dummy);

    if (cr != CUDA_SUCCESS) {
        nvdec_log(avctx, AV_LOG_ERROR, "cuMemcpy2D failed: %d", (int)cr);
        p_av_frame_unref(frame);
        return AVERROR_EXTERNAL;
    }

    s->frames_decoded++;
    return 0;
}

void avcodec_flush_buffers(AVCodecContext *avctx) {
    if (!ensure_orig_loaded())
        return;

    NvdecSession *s = find_session(avctx);
    if (s) {
        s->q_head = 0;
        s->q_count = 0;
        CUVIDSOURCEDATAPACKET cupkt;
        memset(&cupkt, 0, sizeof(cupkt));
        cupkt.flags = CUVID_PKT_DISCONTINUITY;
        CUcontext dummy = NULL;
        g_cu->cuCtxPushCurrent(g_cu_ctx);
        g_cv->cuvidParseVideoData(s->parser, &cupkt);
        g_cu->cuCtxPopCurrent(&dummy);
        s->q_head = 0;
        s->q_count = 0;
    }
    p_orig_flush_buffers(avctx);
}

void avcodec_free_context(AVCodecContext **pavctx) {
    if (!ensure_orig_loaded())
        return;
    if (pavctx && *pavctx) {
        NvdecSession *s = unregister_session(*pavctx);
        if (s)
            destroy_session(s);
    }
    p_orig_free_context(pavctx);
}

/* ---- Qt6Gui!QGuiApplication::focusWindow() IAT hook for Wine/Xwayland controller focus ----
 * On headless Linux (Wine + Xwayland + KWin), fullscreen transitions, DXGI swapchain
 * recreation, or Moonlight reconnects can drop WM_ACTIVATE/WM_SETFOCUS so Qt's
 * QGuiApplication::focusWindow() returns NULL and RemotePlay.cpp stops forwarding
 * DualSense/gamepad input until the user physically clicks the window.
 */
typedef void *(*fn_qt_focusWindow)(void);
static fn_qt_focusWindow p_orig_focusWindow = NULL;
static void *g_last_qwindow = NULL;
static HWND g_last_hwnd = NULL;
static ULONGLONG g_last_nudge_ms = 0;
static int g_focus_logged = 0;

typedef struct FindWinCtx {
    DWORD pid;
    HWND found;
} FindWinCtx;

static BOOL CALLBACK enum_veyra_win_cb(HWND hwnd, LPARAM lp) {
    FindWinCtx *ctx = (FindWinCtx *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    char title[128] = {0};
    GetWindowTextA(hwnd, title, sizeof(title) - 1);
    if (strncmp(title, "Veyra ", 6) == 0 && title[6] >= '0' && title[6] <= '9') {
        ctx->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

static HWND find_veyra_main_hwnd(void) {
    if (g_last_hwnd && IsWindow(g_last_hwnd) && IsWindowVisible(g_last_hwnd) && !IsIconic(g_last_hwnd))
        return g_last_hwnd;
    FindWinCtx ctx = { GetCurrentProcessId(), NULL };
    EnumWindows(enum_veyra_win_cb, (LPARAM)&ctx);
    if (ctx.found) g_last_hwnd = ctx.found;
    return ctx.found;
}

static void *hook_qt_focusWindow(void) {
    if (!p_orig_focusWindow) return NULL;
    void *w = p_orig_focusWindow();
    if (w) {
        g_last_qwindow = w;
        if (!g_last_hwnd) find_veyra_main_hwnd();
        return w;
    }

    HWND hwnd = find_veyra_main_hwnd();
    if (!hwnd) {
        g_last_qwindow = NULL;
        return NULL;
    }

    ULONGLONG now = GetTickCount64();
    if (now - g_last_nudge_ms >= 500) {
        g_last_nudge_ms = now;
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        w = p_orig_focusWindow();
        if (!w) {
            /* Synchronously notify Qt's Windows QPA WndProc so QGuiApplication::focusWindow()
             * binds to Veyra's real QWindow* without needing a synthetic mouse click. */
            SendMessageW(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
            w = p_orig_focusWindow();
        }
        if (w) {
            g_last_qwindow = w;
            if (!g_focus_logged) {
                g_focus_logged = 1;
                ensure_orig_loaded();
                nvdec_log(NULL, AV_LOG_INFO, "auto-focus restored Qt focusWindow=%p for hwnd=%p", w, (void *)hwnd);
            }
            return w;
        }
    }

    if (g_last_qwindow && IsWindow(hwnd)) {
        return g_last_qwindow;
    }
    return NULL;
}

static void install_focus_iat_hook(void) {
    const char *env_dis = getenv("VEYRA_ALWAYS_FOCUS");
    if (env_dis && env_dis[0] == '0') return;

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return;
    uint8_t *base = (uint8_t *)exe;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    IMAGE_DATA_DIRECTORY imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp_dir.VirtualAddress) return;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp_dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        const char *dll_name = (const char *)(base + imp->Name);
        if (_stricmp(dll_name, "Qt6Gui.dll") != 0) continue;
        IMAGE_THUNK_DATA64 *orig_thunk = (IMAGE_THUNK_DATA64 *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA64 *iat_thunk  = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; orig_thunk->u1.AddressOfData; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig_thunk->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig_thunk->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, "?focusWindow@QGuiApplication@@SAPEAVQWindow@@XZ") == 0) {
                DWORD old_prot = 0;
                if (VirtualProtect(&iat_thunk->u1.Function, sizeof(void *), PAGE_READWRITE, &old_prot)) {
                    p_orig_focusWindow = (fn_qt_focusWindow)(uintptr_t)iat_thunk->u1.Function;
                    iat_thunk->u1.Function = (ULONGLONG)(uintptr_t)&hook_qt_focusWindow;
                    VirtualProtect(&iat_thunk->u1.Function, sizeof(void *), old_prot, &old_prot);
                    nvdec_debug_file("installed Qt6Gui!QGuiApplication::focusWindow IAT hook");
                }
                return;
            }
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_lock);
        g_lock_init = 1;
        install_focus_iat_hook();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_lock_init) {
            DeleteCriticalSection(&g_lock);
            g_lock_init = 0;
        }
    }
    return TRUE;
}
