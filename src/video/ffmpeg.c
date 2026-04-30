/*
 * ffmpeg.c — initialisation FFmpeg avec support hwaccel DRM (Pi 5)
 *
 * Sur Pi 5 avec le fork jc-kynesim/rpi-ffmpeg (branch test/7.1.2/main),
 * le décodage HEVC accéléré utilise un hwaccel DRM greffé sur le décodeur
 * logiciel 'hevc' — PAS un décodeur standalone 'hevc_v4l2request'.
 *
 * Pipeline Pi 5 :
 *   avcodec_find_decoder("hevc")
 *   + AVHWDeviceContext AV_HWDEVICE_TYPE_DRM → /dev/dri/card0
 *   + get_format callback → AV_PIX_FMT_DRM_PRIME
 *   → frames exportées comme DMA-BUF vers le display DRM (zero-copy)
 *
 * Pipeline Pi 4 :
 *   avcodec_find_decoder("hevc_v4l2m2m") ou ("h264_v4l2m2m")
 *   → API M2M stateful, frames en mémoire système
 *
 * Moonlight is free software; GPL-3.0+
 */

#include "ffmpeg.h"

#ifdef HAVE_VAAPI
#include "ffmpeg_vaapi.h"
#endif

#include <Limelight.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

/* ── État global ─────────────────────────────────────────────────────────── */

static AVPacket*         pkt         = NULL;
static const AVCodec*    decoder     = NULL;
static AVCodecContext*   decoder_ctx = NULL;
static AVFrame**         dec_frames  = NULL;
static int               dec_frames_cnt = 0;
static int               current_frame  = 0;
static int               next_frame     = 0;
static AVBufferRef*      hw_device_ctx  = NULL; /* contexte hwaccel DRM */

enum decoders ffmpeg_decoder;

/* Hook de format optionnel — positionné par v4l2drm.c avant ffmpeg_init() */
enum AVPixelFormat (*ffmpeg_get_format_cb)(AVCodecContext*,
                                           const enum AVPixelFormat*) = NULL;

/* ── hwaccel DRM (Pi 5 / rpi-ffmpeg) ────────────────────────────────────── */

/*
 * Callback get_format pour le hwaccel DRM.
 * Appelé par FFmpeg pour négocier le format de sortie des frames.
 * On retourne AV_PIX_FMT_DRM_PRIME si disponible (zero-copy vers DRM/KMS),
 * sinon on laisse FFmpeg choisir (fallback YUV en mémoire).
 */
static enum AVPixelFormat drm_hwaccel_get_format(AVCodecContext* ctx,
                                                  const enum AVPixelFormat* fmts) {
  /* Priorité : callback externe (v4l2drm.c) > DRM_PRIME > premier dispo */
  if (ffmpeg_get_format_cb)
    return ffmpeg_get_format_cb(ctx, fmts);

  for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
    if (*p == AV_PIX_FMT_DRM_PRIME) return *p;

  return fmts[0];
}

/*
 * Tente d'initialiser le hwaccel DRM sur le premier device disponible.
 * Retourne 0 si succès, -1 sinon (le décodeur soft sera utilisé quand même).
 */
static int try_init_drm_hwaccel(AVCodecContext* ctx) {
  /*
   * Sur Pi 5 :
   *   renderD128 = nœud render V3D (sans droits display)
   *   card0      = V3D GPU (compute)
   *   card1      = contrôleur HDMI (display) ← essayer en dernier
   *
   * av_hwdevice_ctx_create AV_HWDEVICE_TYPE_DRM ouvre juste un fd
   * sur le device DRM et l'utilise pour allouer des DMA-BUF partagés.
   * Sur Pi 5 avec le fork jc-kynesim, card1 (le display controller)
   * est souvent le bon device car c'est lui qui importe les DMA-BUF
   * du décodeur rpivid vers le framebuffer HDMI.
   */
  const char* drm_devs[] = {
    "/dev/dri/renderD128",
    "/dev/dri/card0",
    "/dev/dri/card1",
    NULL
  };

  for (int i = 0; drm_devs[i]; i++) {
    if (access(drm_devs[i], F_OK) != 0) {
      printf("FFmpeg: DRM hwaccel: %s absent\n", drm_devs[i]);
      continue;
    }

    int ret = av_hwdevice_ctx_create(&hw_device_ctx,
                                     AV_HWDEVICE_TYPE_DRM,
                                     drm_devs[i], NULL, 0);
    if (ret == 0) {
      ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
      ctx->get_format    = drm_hwaccel_get_format;
      printf("FFmpeg: DRM hwaccel on %s\n", drm_devs[i]);
      return 0;
    }

    char errbuf[128];
    av_strerror(ret, errbuf, sizeof(errbuf));
    printf("FFmpeg: DRM hwaccel %s failed: %s\n", drm_devs[i], errbuf);
  }

  /* Dernier essai : auto-détection FFmpeg (NULL = premier device DRM) */
  int ret = av_hwdevice_ctx_create(&hw_device_ctx,
                                   AV_HWDEVICE_TYPE_DRM,
                                   NULL, NULL, 0);
  if (ret == 0) {
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    ctx->get_format    = drm_hwaccel_get_format;
    printf("FFmpeg: DRM hwaccel via auto-detection\n");
    return 0;
  }

  char errbuf[128];
  av_strerror(ret, errbuf, sizeof(errbuf));
  fprintf(stderr, "FFmpeg: DRM hwaccel unavailable: %s\n", errbuf);
  return -1;
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

int ffmpeg_init(int videoFormat, int width, int height,
                int perf_lvl, int buffer_count, int thread_count) {
  av_log_set_level(AV_LOG_WARNING);
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58,10,100)
  avcodec_register_all();
#endif

  pkt = av_packet_alloc();
  if (!pkt) { fprintf(stderr, "FFmpeg: av_packet_alloc failed\n"); return -1; }

  ffmpeg_decoder = (perf_lvl & VAAPI_ACCELERATION) ? VAAPI : SOFTWARE;

  /*
   * Sélection du décodeur par ordre de priorité :
   *
   * HEVC :
   *   1. hevc_v4l2request (Pi 5 fork, si compilé comme décodeur standalone)
   *   2. hevc             (Pi 5 fork, avec hwaccel DRM — cas le plus courant)
   *   3. hevc_v4l2m2m     (Pi 4 bcm2835-codec, API stateful)
   *   4. hevc             (soft decode pur, sans hwaccel)
   *
   * H264 :
   *   1. h264_v4l2m2m     (Pi 4 bcm2835-codec)
   *   2. h264             (soft decode — Pi 5 n'a pas h264_v4l2request)
   *
   * NB : Pour HEVC sur Pi 5, on tente hevc_v4l2request d'abord au cas où
   * le fork l'aurait compilé en standalone dans une version future.
   * Si absent, on utilise 'hevc' + hwaccel DRM.
   */
  decoder = NULL;
  decoder_ctx = NULL;

  for (int try = 0; try < 8 && !decoder; try++) {
    const AVCodec* candidate = NULL;
    bool           need_hwaccel = false; /* true = configure DRM hwaccel */

    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
      if (ffmpeg_decoder == SOFTWARE) {
        if (try == 0) candidate = avcodec_find_decoder_by_name("h264_v4l2m2m");
        if (try == 1) candidate = avcodec_find_decoder_by_name("h264_nvv4l2");
        if (try == 2) candidate = avcodec_find_decoder_by_name("h264_nvmpi");
        if (try == 3) candidate = avcodec_find_decoder_by_name("h264_omx");
      }
      if (try == 4) candidate = avcodec_find_decoder_by_name("h264");

    } else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
      if (ffmpeg_decoder == SOFTWARE) {
        /* try 0 : décodeur standalone (fork futur ou autre distro) */
        if (try == 0) candidate = avcodec_find_decoder_by_name("hevc_v4l2request");
        /* try 1 : 'hevc' avec hwaccel DRM — cas Pi 5 rpi-ffmpeg actuel */
        if (try == 1) {
          candidate    = avcodec_find_decoder_by_name("hevc");
          need_hwaccel = true;
        }
        /* try 2 : M2M stateful Pi 4 */
        if (try == 2) candidate = avcodec_find_decoder_by_name("hevc_v4l2m2m");
        if (try == 3) candidate = avcodec_find_decoder_by_name("hevc_nvv4l2");
        if (try == 4) candidate = avcodec_find_decoder_by_name("hevc_nvmpi");
        if (try == 5) candidate = avcodec_find_decoder_by_name("hevc_omx");
      }
      /* try 6 : soft decode pur, sans hwaccel */
      if (try == 6) candidate = avcodec_find_decoder_by_name("hevc");

    } else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
      if (ffmpeg_decoder == SOFTWARE)
        if (try == 0) candidate = avcodec_find_decoder_by_name("libdav1d");
      if (try == 1) candidate = avcodec_find_decoder_by_name("av1");
    } else {
      fprintf(stderr, "FFmpeg: unsupported video format\n");
      return -1;
    }

    if (!candidate) continue;

    AVCodecContext* ctx = avcodec_alloc_context3(candidate);
    if (!ctx) { fprintf(stderr, "FFmpeg: avcodec_alloc_context3 failed\n"); return -1; }

    ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags  |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    ctx->err_recognition = AV_EF_EXPLODE;
    ctx->width   = width;
    ctx->height  = height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (perf_lvl & SLICE_THREADING) {
      ctx->thread_type  = FF_THREAD_SLICE;
      ctx->thread_count = thread_count;
    } else {
      ctx->thread_count = 1;
    }

    /* Application du get_format :
     * - Callback externe (v4l2drm.c) ou hwaccel DRM → seulement si hwaccel dispo
     * - NE PAS appliquer sur le décodeur soft pur (try 6) car sans hw_device_ctx
     *   FFmpeg rejette DRM_PRIME et génère une erreur → swscale sans context
     */
    if (need_hwaccel) {
      /* try_init_drm_hwaccel configure get_format en interne si succès */
      if (try_init_drm_hwaccel(ctx) < 0) {
        avcodec_free_context(&ctx);
        continue; /* essaie hevc_v4l2m2m (try 2) */
      }
    } else if (ffmpeg_get_format_cb && hw_device_ctx) {
      /* Callback externe valide seulement si un contexte hw a été alloué */
      ctx->get_format = ffmpeg_get_format_cb;
    }
    /* Sinon : décodeur software pur, pas de get_format personnalisé */

#ifdef HAVE_VAAPI
    if (ffmpeg_decoder == VAAPI)
      vaapi_init(ctx);
#endif

    int err = avcodec_open2(ctx, candidate, NULL);
    if (err < 0) {
      char errbuf[256];
      av_strerror(err, errbuf, sizeof(errbuf));
      printf("FFmpeg: %s failed: %s\n", candidate->name, errbuf);
      /* Libère aussi le hw_device_ctx si on en avait alloué un */
      if (need_hwaccel && hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
        hw_device_ctx = NULL;
      }
      avcodec_free_context(&ctx);
      continue;
    }

    decoder     = candidate;
    decoder_ctx = ctx;
    /* break implicite : la condition !decoder stoppe la boucle */
  }

  if (!decoder) {
    fprintf(stderr, "FFmpeg: no suitable decoder found\n");
    return -1;
  }

  printf("FFmpeg: using decoder '%s'%s\n",
         decoder->name,
         hw_device_ctx ? " + DRM hwaccel" : "");

  dec_frames_cnt = buffer_count;
  dec_frames = calloc(buffer_count, sizeof(AVFrame*));
  if (!dec_frames) return -1;

  for (int i = 0; i < buffer_count; i++) {
    dec_frames[i] = av_frame_alloc();
    if (!dec_frames[i]) return -1;
  }

  return 0;
}

/* ── Destruction ─────────────────────────────────────────────────────────── */

void ffmpeg_destroy(void) {
  av_packet_free(&pkt);
  if (decoder_ctx) avcodec_free_context(&decoder_ctx);
  if (hw_device_ctx) { av_buffer_unref(&hw_device_ctx); hw_device_ctx = NULL; }
  if (dec_frames) {
    for (int i = 0; i < dec_frames_cnt; i++)
      if (dec_frames[i]) av_frame_free(&dec_frames[i]);
    free(dec_frames);
    dec_frames = NULL;
  }
  ffmpeg_get_format_cb = NULL;
}

/* ── Décodage ────────────────────────────────────────────────────────────── */

AVFrame* ffmpeg_get_frame(bool native_frame) {
  int err = avcodec_receive_frame(decoder_ctx, dec_frames[next_frame]);
  if (err == 0) {
    current_frame = next_frame;
    next_frame    = (current_frame + 1) % dec_frames_cnt;
    if (ffmpeg_decoder == SOFTWARE || native_frame)
      return dec_frames[current_frame];
  } else if (err != AVERROR(EAGAIN)) {
    char errbuf[256];
    av_strerror(err, errbuf, sizeof(errbuf));
    fprintf(stderr, "FFmpeg: receive failed: %s\n", errbuf);
  }
  return NULL;
}

int ffmpeg_decode(unsigned char* indata, int inlen) {
  pkt->data = indata;
  pkt->size = inlen;
  int err = avcodec_send_packet(decoder_ctx, pkt);
  if (err < 0) {
    char errbuf[256];
    av_strerror(err, errbuf, sizeof(errbuf));
    fprintf(stderr, "FFmpeg: decode failed: %s\n", errbuf);
  }
  return err < 0 ? err : 0;
}
