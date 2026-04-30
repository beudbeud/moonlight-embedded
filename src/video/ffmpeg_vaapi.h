/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 * Fix #3/#4 : signature Display*, détection format 10-bit
 * Fix F     : détection AV1 dynamique via vaQueryConfigProfiles()
 *
 * Moonlight is free software; GPL-3.0+
 */

#pragma once

#include <libavcodec/avcodec.h>
#include <X11/Xlib.h>
#include <stdbool.h>

/*
 * Fix #3 : accepte le Display* ouvert par x11.c.
 * Tente en cascade : nœuds DRM render → auto-détection → ":0".
 * Retourne 0 en cas de succès, -1 si tous les chemins échouent.
 */
int vaapi_init_lib(Display* display);

int  vaapi_init(AVCodecContext* decoder_ctx);
void vaapi_queue(AVFrame* dec_frame, Window win, int width, int height);

/*
 * Fix F : indique si le GPU courant supporte le décodage AV1 via VAAPI.
 * Retourne false si vaapi_init_lib() n'a pas encore été appelé, ou si
 * VAProfileAV1Profile0 n'est pas dans la liste des profils supportés.
 * Compilé conditionnellement si libva >= 2.11 (VAProfileAV1Profile0 défini).
 */
bool vaapi_has_av1(void);
