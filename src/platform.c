/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 * FIX #2 : détection et dispatch du backend V4L2_DRM
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE

#include "platform.h"
#include "util.h"
#include "audio/audio.h"
#include "video/video.h"

/* Fix F : vaapi_has_av1() utilisé dans platform_prefers_codec() */
#ifdef HAVE_VAAPI
#include "video/ffmpeg_vaapi.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

typedef bool(*ImxInit)();

enum platform platform_check(char* name) {
  bool std = strcmp(name, "auto") == 0;

#ifdef HAVE_IMX
  if (std || strcmp(name, "imx") == 0) {
    void *handle = dlopen("libmoonlight-imx.so", RTLD_NOW | RTLD_GLOBAL);
    ImxInit video_imx_init = (ImxInit) dlsym(RTLD_DEFAULT, "video_imx_init");
    if (handle != NULL && video_imx_init())
      return IMX;
  }
#endif

#ifdef HAVE_PI
  if (std || strcmp(name, "pi") == 0) {
    void *handle = dlopen("libmoonlight-pi.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle != NULL && dlsym(RTLD_DEFAULT, "bcm_host_init") != NULL)
      return PI;
  }
#endif

#ifdef HAVE_MMAL
  if (std || strcmp(name, "mmal") == 0) {
    void *handle = dlopen("libmoonlight-mmal.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle != NULL && dlsym(RTLD_DEFAULT, "bcm_host_init") != NULL)
      return MMAL;
  }
#endif

  /*
   * FIX #2 : détection V4L2_DRM pour Raspberry Pi 5 et SoC Linux modernes.
   *
   * Critères (tous optionnels, OR logique — on accepte dès qu'un des
   * devices est présent, car leur disponibilité dépend du noyau/distro) :
   *   /dev/video10  → décodeur H.264 exposé par le driver rp1 du Pi 5
   *   /dev/video11  → décodeur HEVC exposé par le driver rp1 du Pi 5
   *   /dev/dri/card0 → device DRM/KMS pour le rendu
   *
   * NB : on vérifie intentionnellement l'absence de bcm_host_init pour
   * s'assurer qu'on n'entre pas dans ce chemin sur Pi 1-4 avec /opt/vc,
   * où MMAL est préférable.
   */
#ifdef HAVE_V4L2_DRM
  if (std || strcmp(name, "v4l2drm") == 0) {
    bool has_drm = (access("/dev/dri/card0", F_OK) == 0 ||
                    access("/dev/dri/card1", F_OK) == 0);
    bool has_bcm = (dlsym(RTLD_DEFAULT, "bcm_host_init") != NULL);

    if (std) {
      /*
       * Mode auto : détection stricte.
       * Exige un décodeur V4L2 M2M connu ET un device DRM, sans Broadcom.
       * Évite d'activer le backend sur des boards sans décodeur matériel
       * (ex. Pi 3 qui a /dev/dri/card0 mais pas de V4L2 codec).
       */
      bool has_v4l2 = (access("/dev/video10", F_OK) == 0 ||
                       access("/dev/video11", F_OK) == 0 ||
                       access("/dev/video18", F_OK) == 0 || /* Pi 5 rp1 */
                       access("/dev/video19", F_OK) == 0);
      if (has_v4l2 && has_drm && !has_bcm)
        return V4L2_DRM;
    } else {
      /*
       * Mode explicite (-platform v4l2drm) :
       * L'utilisateur sait ce qu'il fait. On vérifie seulement que DRM
       * est disponible — pas de check V4L2 car le numéro de device varie
       * selon le kernel (video10, video18, video19...).
       * Le décodeur FFmpeg sera sélectionné au runtime par ffmpeg_init().
       */
      if (has_drm)
        return V4L2_DRM;
    }
  }
#endif

#ifdef HAVE_AML
  if (std || strcmp(name, "aml") == 0) {
    void *handle = dlopen("libmoonlight-aml.so", RTLD_LAZY | RTLD_GLOBAL);
    if (handle != NULL && access("/dev/amvideo", F_OK) != -1)
      return AML;
  }
#endif

#ifdef HAVE_ROCKCHIP
  if (std || strcmp(name, "rk") == 0) {
    void *handle = dlopen("libmoonlight-rk.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle != NULL && dlsym(RTLD_DEFAULT, "mpp_init") != NULL)
      return RK;
  }
#endif

#ifdef HAVE_X11
  bool x11   = strcmp(name, "x11")       == 0;
  bool vdpau = strcmp(name, "x11_vdpau") == 0;
  bool vaapi = strcmp(name, "x11_vaapi") == 0;
  if (std || x11 || vdpau || vaapi) {
    int init = x11_init(std || vdpau, std || vaapi);
#ifdef HAVE_VAAPI
    if (init == INIT_VAAPI)  return X11_VAAPI;
#endif
#ifdef HAVE_VDPAU
    if (init == INIT_VDPAU)  return X11_VDPAU;
#endif
#ifdef HAVE_SDL
    return SDL;
#else
    return X11;
#endif
  }
#endif

#ifdef HAVE_SDL
  if (std || strcmp(name, "sdl") == 0)
    return SDL;
#endif

  if (strcmp(name, "fake") == 0)
    return FAKE;

  return 0;
}

void platform_start(enum platform system) {
  switch (system) {
#ifdef HAVE_AML
  case AML:
    write_bool("/sys/class/graphics/fb0/blank",    true);
    write_bool("/sys/class/graphics/fb1/blank",    true);
    write_bool("/sys/class/video/disable_video",   false);
    break;
#endif
#if defined(HAVE_PI) || defined(HAVE_MMAL)
  case PI:
    write_bool("/sys/class/graphics/fb0/blank", true);
    break;
#endif
  // V4L2_DRM gère lui-même le display via DRM — pas de sysfs nécessaire
  default:
    break;
  }
}

void platform_stop(enum platform system) {
  switch (system) {
#ifdef HAVE_AML
  case AML:
    write_bool("/sys/class/graphics/fb0/blank", false);
    write_bool("/sys/class/graphics/fb1/blank", false);
    break;
#endif
#if defined(HAVE_PI) || defined(HAVE_MMAL)
  case PI:
    write_bool("/sys/class/graphics/fb0/blank", false);
    break;
#endif
  default:
    break;
  }
}

DECODER_RENDERER_CALLBACKS* platform_get_video(enum platform system) {
  switch (system) {
#ifdef HAVE_X11
  case X11:
    return &decoder_callbacks_x11;
#ifdef HAVE_VAAPI
  case X11_VAAPI:
    return &decoder_callbacks_x11_vaapi;
#endif
#ifdef HAVE_VDPAU
  case X11_VDPAU:
    return &decoder_callbacks_x11_vdpau;
#endif
#endif
#ifdef HAVE_SDL
  case SDL:
    return &decoder_callbacks_sdl;
#endif
#ifdef HAVE_IMX
  case IMX:
    return (PDECODER_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "decoder_callbacks_imx");
#endif
#ifdef HAVE_PI
  case PI:
    return (PDECODER_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "decoder_callbacks_pi");
#endif
#ifdef HAVE_MMAL
  case MMAL:
    return (PDECODER_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "decoder_callbacks_mmal");
#endif
#ifdef HAVE_AML
  case AML:
    return (PDECODER_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "decoder_callbacks_aml");
#endif
#ifdef HAVE_ROCKCHIP
  case RK:
    return (PDECODER_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "decoder_callbacks_rk");
#endif
#ifdef HAVE_V4L2_DRM
  // FIX #2 : dispatch vers le nouveau backend Pi 5
  case V4L2_DRM:
    return &decoder_callbacks_v4l2drm;
#endif
  }
  return NULL;
}

AUDIO_RENDERER_CALLBACKS* platform_get_audio(enum platform system,
                                             char* audio_device) {
  if (audio_device && strcmp(audio_device, "nosound") == 0)
    return NULL;

#ifdef HAVE_SDL
  if (audio_device && strcmp(audio_device, "sdl") == 0)
    return &audio_callbacks_sdl;
#endif

  switch (system) {
  case FAKE:
    return NULL;
#ifdef HAVE_SDL
  case SDL:
    return &audio_callbacks_sdl;
#endif
#ifdef HAVE_V4L2_DRM
  case V4L2_DRM:
    /* On embedded systems, direct ALSA is often unavailable (PulseAudio holds
     * the device, or the plug plugin is missing). Try PA first, then SDL which
     * handles PA/OSS/ALSA transparently, before falling back to raw ALSA. */
#ifdef HAVE_PULSE
    if (audio_pulse_init(audio_device)) return &audio_callbacks_pulse;
#endif
#ifdef HAVE_SDL
    return &audio_callbacks_sdl;
#endif
#ifdef HAVE_ALSA
    return &audio_callbacks_alsa;
#endif
    return NULL;
#endif
#ifdef HAVE_PI
  case PI:
    if (audio_device == NULL ||
        strcmp(audio_device, "local") == 0 ||
        strcmp(audio_device, "hdmi")  == 0)
      return (PAUDIO_RENDERER_CALLBACKS) dlsym(RTLD_DEFAULT, "audio_callbacks_omx");
    // fall-through
#endif
  default:
#ifdef HAVE_PULSE
    if (audio_pulse_init(audio_device))
      return &audio_callbacks_pulse;
#endif
#ifdef HAVE_ALSA
    return &audio_callbacks_alsa;
#endif
#ifdef __FreeBSD__
    return &audio_callbacks_oss;
#endif
  }
  return NULL;
}

bool platform_prefers_codec(enum platform system, enum codecs codec) {
  switch (codec) {
  case CODEC_H264:
    /*
     * Pi 5 (V4L2_DRM) : le fork rpi-ffmpeg (jc-kynesim, branch
     * test/7.1.2/main) ne fournit PAS h264_v4l2request.
     * Seul HEVC est accéléré via hevc_v4l2request + rpivid (/dev/video19).
     * On retourne false pour que Moonlight négocie HEVC avec Sunshine
     * plutôt que H.264 qui tomberait en soft decode.
     */
    return (system != V4L2_DRM);

  case CODEC_HEVC:
    switch (system) {
    case AML:
    case RK:
    case X11_VAAPI:
    case X11_VDPAU:
    case V4L2_DRM:  /* Pi 5 : hevc_v4l2request via rpivid/dev/video19 */
    case MMAL:      /* Pi 4 : MMAL_ENCODING_H265                       */
      return true;
    default:
      return false;
    }

  case CODEC_AV1:
    /*
     * Fix F : détection dynamique via vaQueryConfigProfiles().
     * vaapi_has_av1() retourne true si le GPU courant supporte
     * VAProfileAV1Profile0 (Intel ARC, AMD RDNA2+, NVIDIA Ada).
     * Retourne false si VAAPI n'est pas initialisé ou si libva < 2.11.
     */
    switch (system) {
#ifdef HAVE_VAAPI
    case X11_VAAPI:
      return vaapi_has_av1();
#endif
    default:
      return false;
    }

  default:
    return false;
  }
}

char* platform_name(enum platform system) {
  switch (system) {
  case PI:        return "Raspberry Pi (Broadcom OMX)";
  case MMAL:      return "Raspberry Pi (Broadcom MMAL)";
  case V4L2_DRM:  return "Linux V4L2 M2M + DRM/KMS (Pi 5 / SoC moderne)";
  case IMX:       return "i.MX6 (MXC Vivante)";
  case AML:       return "AMLogic VPU";
  case RK:        return "Rockchip VPU";
  case X11:       return "X Window System (décodage logiciel)";
  case X11_VAAPI: return "X Window System (VAAPI)";
  case X11_VDPAU: return "X Window System (VDPAU)";
  case SDL:       return "SDL2 (décodage logiciel)";
  case FAKE:      return "Fake (pas de sortie A/V)";
  default:        return "Inconnu";
  }
}
