# Patches appliqués — Accélération graphique vidéo

## Résumé des corrections

| # | Problème | Fichiers modifiés |
|---|----------|-------------------|
| 1 | `break` manquant dans la sélection du décodeur FFmpeg | `src/video/ffmpeg.c` |
| 2 | Pas de backend pour Raspberry Pi 5 (V4L2 M2M + DRM/KMS) | `src/video/v4l2drm.c` *(nouveau)*, `src/platform.c`, `src/platform.h`, `src/video/video.h`, `CMakeLists.txt` |
| 3 | `vaapi_init_lib()` ignore le Display* et force `:0` en dur | `src/video/ffmpeg_vaapi.c`, `src/video/ffmpeg_vaapi.h` |
| 4 | Format surface VAAPI câblé sur NV12 (pas de 10 bits / HDR) | `src/video/ffmpeg_vaapi.c` |
| 5 | Rendu EGL v4l2drm : image tuilée, couleurs fausses, image inversée | `src/video/v4l2drm.c` |
| 6 | Chemin DRM direct zero-copy (NV12+SAND128 → moteur d'affichage) | `src/video/v4l2drm.c` |
| 7 | Audio SDL sélectionnable pour tout backend (`-audio sdl`) | `src/platform.c` |

---

## Détail des correctifs

### Fix #1 — `break` manquant dans la boucle de sélection du décodeur (`ffmpeg.c`)

**Symptôme :** tout décodeur matériel sélectionné (ex. `h264_v4l2m2m` à try=3)
était systématiquement écrasé par le décodeur logiciel générique `h264` (try=4),
rendant toute accélération matérielle inopérante.

**Correction :** ajout d'un `break` immédiatement après `avcodec_open2()` en cas
de succès. La boucle est également refactorisée pour utiliser une variable
`candidate` distincte (plus lisible, évite les effets de bord entre itérations).

---

### Fix #2 — Backend V4L2 M2M + DRM/KMS pour Raspberry Pi 5 (`v4l2drm.c`)

**Symptôme :** le Pi 5 utilise le GPU VideoCore VII sans bibliothèques Broadcom
`/opt/vc`. Les backends `PI` et `MMAL` vérifient `bcm_host_init` via `dlsym` —
symbole absent sur Pi 5 — et tombent en fallback SDL (décodage CPU pur).

**Correction :** nouveau backend `V4L2_DRM` compilé directement dans le binaire
`moonlight` (comme X11/SDL) :
- **Décodage :** FFmpeg avec `h264_v4l2m2m` / `hevc_v4l2m2m` (driver kernel `rp1`)
- **Rendu :** DRM/KMS via `libdrm` avec double-buffering (dumb buffers XRGB8888)
- **Conversion :** `libswscale` (YUV420P/NV12 → XRGB8888)
- **Détection :** présence de `/dev/video10`, `/dev/video11` ou `/dev/dri/card0`,
  combinée à l'absence de `bcm_host_init` (pour ne pas interférer avec Pi 1-4)
- **HEVC :** `platform_prefers_codec()` retourne `true` pour `V4L2_DRM`
- **Capabilities :** `CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC` activé

**Dépendances à installer sur Pi 5 (Raspberry Pi OS / Ubuntu) :**
```bash
sudo apt install libdrm-dev libswscale-dev libavcodec-dev libavutil-dev
```

**Limitations connues (v1) :**
- Transfert CPU via swscale (une version future utilisera DMA-BUF/DRM_PRIME zero-copy)
- Un seul connecteur (premier connecteur connecté trouvé)
- Pas de drmModePageFlip (mode-set complet à chaque frame — fonctionnel mais non optimal)

---

### Fix #3 — Signature de `vaapi_init_lib()` et détection robuste (`ffmpeg_vaapi.c/h`)

**Symptôme :** `vaapi_init_lib()` était déclaré sans paramètre mais appelé avec
un `Display*` depuis `x11.c` (comportement indéfini en C). La fonction ignorait
le display passé et ouvrait toujours `:0` en dur, échouant sur :
- Sessions Wayland (DISPLAY != `:0` ou inexistant)
- Configurations multi-écrans (`DISPLAY=:1`)
- Systèmes sans serveur X démarré au moment de l'init

**Correction :** cascade de détection en 3 étapes :
1. Nœuds DRM render (`/dev/dri/renderD128` … `renderD131`) — chemin préféré
2. Auto-détection FFmpeg (`NULL`) — fallback générique
3. Display X11 `:0` — comportement original en dernier recours

---

### Fix #4 — Support 10 bits / HDR via VAAPI (`ffmpeg_vaapi.c`)

**Symptôme :** `fr_ctx->sw_format` était câblé sur `AV_PIX_FMT_NV12` (8 bits),
empêchant le décodage de contenu HEVC Main 10 / HDR même sur des GPU VAAPI
compatibles (Intel Gen 9+, AMD GCN4+). Le décodeur retournait une erreur
`hwframe_ctx_init` ou produisait des artefacts de couleur.

**Correction :** fonction `select_sw_format()` qui détecte le profil 10 bits via :
- `context->profile == FF_PROFILE_HEVC_MAIN_10` ou `FF_PROFILE_HEVC_REXT`
- `context->sw_pix_fmt` dans `{YUV420P10LE, YUV420P10BE, P010LE, P010BE}`

et sélectionne `AV_PIX_FMT_P010` en conséquence.

---

### Fix #5 — Rendu EGL v4l2drm : image tuilée, couleurs fausses, image inversée (`v4l2drm.c`)

**Symptômes :** trois bugs distincts dans le chemin de rendu EGL du backend V4L2_DRM :

1. **Image tuilée ("petits écrans")** — Le décodeur rpivid du Pi 5 sort les frames HEVC
   en format SAND128 (colonnes tuilées de 128 px, modifier DRM `0x0700000000066004`).
   L'import EGL ne passait pas les attributs `EGL_DMA_BUF_PLANE_MODIFIER_{LO,HI}_EXT` ;
   Mesa interprétait les données tuilées comme du NV12 linéaire, produisant un motif
   de répétition.

2. **Couleurs fausses (vert/orange puis magenta/vert)** — Le shader GLSL appliquait
   manuellement une conversion BT.601 YCbCr→RGB sur des données que Mesa V3D convertit
   déjà en hardware via son TMU (double conversion). De plus, `glReadPixels(GL_RGBA)`
   écrit `[R,G,B,A]` en mémoire alors que `DRM_FORMAT_XRGB8888` attend `[B,G,R,X]`
   (little-endian) : les canaux R et B étaient inversés dans l'affichage.

3. **Image à l'envers** — La boucle de copie readback→dumb buffer effectuait un flip
   vertical inutile. Avec le modifier SAND128 correctement transmis, Mesa V3D adopte
   la convention DMA-BUF (UV(0,0) = haut-gauche) ; combinée au mapping du quad EGL
   (UV(0,0) au bas de l'écran OpenGL), l'image était déjà droite à la sortie de
   `glReadPixels` — le flip supplémentaire la retournait une seconde fois.

**Corrections :**
- Ajout des constantes `EGL_DMA_BUF_PLANE{0,1}_MODIFIER_{LO,HI}_EXT` et détection
  de l'extension `EGL_EXT_image_dma_buf_import_modifiers` dans `egl_init()`.
- Passage du modifier DRM (`desc->objects[i].format_modifier`) à `eglCreateImageKHR`
  pour chaque plan de la frame (log du format/modifier au premier appel pour debug).
- Shader fragment simplifié : passthrough Mesa RGB + swap `vec4(c.b, c.g, c.r, 1.0)`
  pour corriger l'ordre des octets vis-à-vis de `DRM_FORMAT_XRGB8888`.
- Suppression du flip vertical dans la boucle de copie readback→dumb buffer (correction
  aussi du mismatch stride `W×4` vs `buf->stride` via un buffer intermédiaire).

---

### Fix #6 — Chemin DRM direct zero-copy NV12+SAND128 (`v4l2drm.c`)

**Objectif :** contourner entièrement le GPU (pas d'EGL, pas de swscale) en transmettant
les frames NV12+SAND128 directement au moteur d'affichage du Pi 5 (HVS/RP1) via KMS.

**Pipeline :** `hevc_v4l2request` → `AVFrame DRM_PRIME` → `drmModeAddFB2WithModifiers`
→ `drmModePageFlip` — aucune copie CPU ni GPU.

**Problème rencontré :** `drmModePageFlip` retournait `EINVAL` sur toutes les frames.
Le ioctl "legacy" PageFlip ne peut pas changer le pixel format du plan primaire
(XRGB8888 → NV12). Ce n'est possible qu'avec une revalidation atomique ou un `SetCrtc`.

**Correction :**
- **Première frame NV12** : `drmModeSetCrtc(NV12+SAND128)` pour établir le format
  du plan primaire (SetCrtc est synchrone et gère les changements de format via
  l'atomic helper interne du driver vc4).
- **Frames suivantes** : `drmModePageFlip(NV12→NV12)` avec événement vblank pour
  la synchronisation verticale ; `page_flip_handler` libère la frame précédente
  (`drmModeRmFB` + `av_frame_free`) dès que l'affichage a basculé.
- **Détection** : `drmGetCap(DRM_CAP_ADDFB2_MODIFIERS)` au démarrage ; fallback
  transparent vers EGL si le chemin direct échoue.
- **Gestion GEM** : `drmPrimeFDToHandle` + `DRM_IOCTL_GEM_CLOSE` immédiat après
  `drmModeAddFB2` (KMS conserve sa propre référence).

**Résultat :** log au démarrage `"direct path active — NV12+SAND128 → display engine"`,
zéro copie CPU, zéro GPU utilisé pour le rendu vidéo.

---

### Fix #7 — Audio SDL sélectionnable pour tous les backends (`platform.c`)

**Symptôme :** sur Recalbox RGB Dual 2 (et tout système avec ALSA minimal), le backend
`alsa.c` échoue à ouvrir `hw:0,0` ou `plughw:0,0` car le plugin `libasound_module_pcm_plug.so`
est absent. SDL audio fonctionne sur le même hardware car SDL négocie les paramètres
différemment (`SDL_AUDIO_ALLOW_*`).

**Correction :** ajout d'un test `strcmp(audio_device, "sdl")` en entrée de
`platform_get_audio()`. N'importe quel backend vidéo (V4L2_DRM, X11…) peut désormais
utiliser SDL audio via `-audio sdl` ou `audio = sdl` dans la config.

**Usage :**
```ini
# moonlight.conf ou hosts/<ip>.conf
audio = sdl
```

---

## Compilation

```bash
mkdir build && cd build
cmake .. -DENABLE_V4L2_DRM=ON
make -j$(nproc)
sudo make install
```

Sur Pi 5 sans X11 :
```bash
cmake .. -DENABLE_X11=OFF -DENABLE_SDL=OFF -DENABLE_V4L2_DRM=ON
```
