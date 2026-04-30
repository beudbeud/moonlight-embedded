/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Fixes :
 *   - Buffer porté de 60 ms à 200 ms pour absorber les irrégularités
 *     dues au packet loss réseau sans générer d'underrun.
 *   - Après recover(), attente active si EAGAIN plutôt que drop silencieux.
 *   - Ouverture en mode bloquant (suppression de SND_PCM_NONBLOCK) :
 *     writei bloque au maximum ~20 ms (1 période), ce qui est acceptable
 *     dans le thread audio dédié et évite tous les EAGAIN.
 *
 * Moonlight is free software; GPL-3.0+
 */

#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <opus_multistream.h>
#include <alsa/asoundlib.h>


static snd_pcm_t*       handle       = NULL;
static OpusMSDecoder*   decoder      = NULL;
static short*           pcmBuffer    = NULL;
static int              samplesPerFrame;

static int alsa_renderer_init(int audioConfiguration,
                               POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                               void* context, int arFlags) {
  unsigned char alsaMapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];

  /* Remap FL-FR-C-LFE-RL-RR → FL-FR-RL-RR-C-LFE (ordre ALSA) */
  memcpy(alsaMapping, opusConfig->mapping, sizeof(alsaMapping));
  if (opusConfig->channelCount >= 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }

  samplesPerFrame = opusConfig->samplesPerFrame;
  pcmBuffer = malloc(sizeof(short) * opusConfig->channelCount * samplesPerFrame);
  if (!pcmBuffer) return -1;

  int opus_err = 0;
  decoder = opus_multistream_decoder_create(
    opusConfig->sampleRate, opusConfig->channelCount,
    opusConfig->streams, opusConfig->coupledStreams,
    alsaMapping, &opus_err);
  if (!decoder) { printf("ALSA: opus_multistream_decoder_create failed: %d\n", opus_err); return -1; }

  unsigned int sampleRate  = opusConfig->sampleRate;
  unsigned int pcmChannels = opusConfig->channelCount;

  char* audio_device = (char*)context;
  if (!audio_device) audio_device = "default";

  /* Fallback chain: user device → "default" → "hw:0,0" */
  static const char* const fallbacks[] = { "default", "hw:0,0", NULL };
  int rc_open = snd_pcm_open(&handle, audio_device,
                              SND_PCM_STREAM_PLAYBACK, 0);
  for (int i = 0; rc_open < 0 && fallbacks[i]; i++) {
    if (strcmp(audio_device, fallbacks[i]) == 0)
      continue;
    printf("ALSA: cannot open '%s' (%s), trying '%s'\n",
           audio_device, snd_strerror(rc_open), fallbacks[i]);
    audio_device = fallbacks[i];
    rc_open = snd_pcm_open(&handle, audio_device,
                            SND_PCM_STREAM_PLAYBACK, 0);
  }
  if (rc_open < 0) {
    printf("ALSA: cannot open any audio device: %s\n", snd_strerror(rc_open));
    return -1;
  }

  /*
   * snd_pcm_set_params() configure format, accès, canaux, taux et latence
   * en une seule passe. Il laisse ALSA choisir la période optimale pour le
   * hardware (contraintes DMA I2S, puissance de 2, etc.) plutôt que de
   * forcer 960 frames qui peut être rejeté par certains drivers I2S.
   * soft_resample=1 permet un resampling software si le taux exact n'est
   * pas supporté (rare à 48000 Hz mais défensif).
   * Latence cible : 200 ms (absorbe les irrégularités réseau).
   */
  int rc = snd_pcm_set_params(handle,
                               SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED,
                               pcmChannels, sampleRate,
                               1,       /* soft_resample */
                               200000); /* latence µs = 200 ms */
  if (rc < 0 && pcmChannels > 2) {
    /* Hardware (e.g. PCM5102A I2S DAC) is stereo-only: rebuild decoder for FL+FR. */
    printf("ALSA: %u channels not supported (%s), falling back to stereo\n",
           pcmChannels, snd_strerror(rc));
    pcmChannels = 2;
    rc = snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             2, sampleRate, 1, 200000);
    if (rc == 0) {
      opus_multistream_decoder_destroy(decoder);
      unsigned char stereo_map[2] = { 0, 1 };
      /* Keep original stream/coupled counts — only reduce output channels to 2.
       * Opus discards the extra channels (C, LFE, RL, RR) at the remapper stage. */
      decoder = opus_multistream_decoder_create(sampleRate, 2,
          opusConfig->streams, opusConfig->coupledStreams,
          stereo_map, &opus_err);
      if (!decoder) {
        printf("ALSA: stereo decoder creation failed: %d\n", opus_err);
        snd_pcm_close(handle); handle = NULL; return -1;
      }
      free(pcmBuffer);
      pcmBuffer = malloc(sizeof(short) * 2 * samplesPerFrame);
      if (!pcmBuffer) { snd_pcm_close(handle); handle = NULL; return -1; }
      printf("ALSA: stereo fallback active (FL+FR from %u-ch stream)\n",
             opusConfig->channelCount);
    }
  }
  if (rc < 0) {
    printf("ALSA: snd_pcm_set_params failed on '%s': %s\n",
           audio_device, snd_strerror(rc));
    snd_pcm_close(handle);
    handle = NULL;
    return -1;
  }

  /* Log des paramètres effectifs */
  snd_pcm_uframes_t actual_buf = 0, actual_period = 0;
  snd_pcm_get_params(handle, &actual_buf, &actual_period);
  printf("ALSA: %s | %d Hz | %u ch | period=%lu frames | buffer=%lu frames (%.0f ms)\n",
         audio_device, sampleRate, pcmChannels,
         actual_period, actual_buf,
         (double)actual_buf * 1000.0 / sampleRate);

  return 0;
}

static void alsa_renderer_cleanup(void) {
  if (decoder) { opus_multistream_decoder_destroy(decoder); decoder = NULL; }
  if (handle)  { snd_pcm_drain(handle); snd_pcm_close(handle); handle = NULL; }
  if (pcmBuffer) { free(pcmBuffer); pcmBuffer = NULL; }
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  int decodeLen = opus_multistream_decode(decoder, data, length,
                                           pcmBuffer, samplesPerFrame, 0);
  if (decodeLen < 0) {
    fprintf(stderr, "ALSA: opus decode error %d\n", decodeLen);
    return;
  }

  int rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
  if (rc < 0) {
    /*
     * Underrun ou suspension : snd_pcm_recover() remet le device en état.
     * On réessaie ensuite une seule fois (mode bloquant → writei va
     * attendre que le buffer ait de la place, pas de EAGAIN).
     */
    rc = snd_pcm_recover(handle, rc, 1 /* silent */);
    if (rc == 0) {
      rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    }
  }

  if (rc < 0)
    fprintf(stderr, "ALSA: writei failed after recover: %s\n", snd_strerror(rc));
  else if (rc != decodeLen)
    fprintf(stderr, "ALSA: short write %d/%d frames\n", rc, decodeLen);
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init                = alsa_renderer_init,
  .cleanup             = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities        = CAPABILITY_DIRECT_SUBMIT |
                         CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
