/* sgensys: Linux audio output support.
 * Copyright (c) 2013, 2017-2018 Joel K. Pettersson
 * <joelkpettersson@gmail.com>.
 *
 * This file and the software of which it is part is distributed under the
 * terms of the GNU Lesser General Public License, either version 3 or (at
 * your option) any later version, WITHOUT ANY WARRANTY, not even of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * View the file COPYING for details, or if missing, see
 * <https://www.gnu.org/licenses/>.
 */

#include "oss.c" /* used in fallback mechanism */
#include <alsa/asoundlib.h>
#define ALSA_NAME_OUT "default"

/*
 * Create instance and return it if successful, otherwise NULL.
 *
 * If the first ALSA call fails, try to create and return instance
 * for OSS instead.
 */
static inline SGS_AudioDev *open_AudioDev_linux(const char *alsa_name,
		const char *oss_name, int oss_mode, uint16_t channels,
		uint32_t *srate) {
	SGS_AudioDev *o;
	uint32_t tmp;
	int err;
	snd_pcm_t *handle = NULL;
	snd_pcm_hw_params_t *params = NULL;

	if ((err = snd_pcm_open(&handle, alsa_name, SND_PCM_STREAM_PLAYBACK,
			0)) < 0) {
		o = open_AudioDev_oss(oss_name, oss_mode, channels, srate);
		if (o) {
			return o;
		}
		SGS_error(NULL, "could neither use ALSA nor OSS");
		goto ERROR;
	}

	if (snd_pcm_hw_params_malloc(&params) < 0)
		goto ERROR;
	tmp = *srate;
	if (!params
	    || (err = snd_pcm_hw_params_any(handle, params)) < 0
	    || (err = snd_pcm_hw_params_set_access(handle, params,
		SND_PCM_ACCESS_RW_INTERLEAVED)) < 0
	    || (err = snd_pcm_hw_params_set_format(handle, params,
		SND_PCM_FORMAT_S16)) < 0
	    || (err = snd_pcm_hw_params_set_channels(handle, params,
		channels)) < 0
	    || (err = snd_pcm_hw_params_set_rate_near(handle, params, &tmp,
		0)) < 0
	    || (err = snd_pcm_hw_params(handle, params)) < 0)
		goto ERROR;
	if (tmp != *srate) {
		SGS_warning("ALSA", "sample rate %d unsupported, using %d",
			*srate, tmp);
		*srate = tmp;
	}

	o = malloc(sizeof(SGS_AudioDev));
	o->ref.handle = handle;
	o->type = TYPE_ALSA;
	o->channels = channels;
	o->srate = *srate;
	return o;

ERROR:
	SGS_error("ALSA", "%s", snd_strerror(err));
	if (handle) snd_pcm_close(handle);
	if (params) snd_pcm_hw_params_free(params);
	SGS_error("ALSA", "configuration for device \"%s\" failed",
		alsa_name);
	return NULL;
}

/*
 * Destroy instance. Close ALSA or OSS device,
 * ending playback in the process.
 */
static inline void close_AudioDev_linux(SGS_AudioDev *o) {
	if (o->type == TYPE_OSS) {
		close_AudioDev_oss(o);
		return;
	}
	
	snd_pcm_drain(o->ref.handle);
	snd_pcm_close(o->ref.handle);
	free(o);
}

/*
 * Write audio, returning true on success, false on any error.
 */
static inline bool AudioDev_linux_write(SGS_AudioDev *o, const int16_t *buf,
		uint32_t samples) {
	if (o->type == TYPE_OSS)
		return AudioDev_oss_write(o, buf, samples);

	snd_pcm_sframes_t written;

	while ((written = snd_pcm_writei(o->ref.handle, buf, samples)) < 0) {
		if (written == -EPIPE) {
			SGS_warning("ALSA", "audio device buffer underrun");
			snd_pcm_prepare(o->ref.handle);
		} else {
			SGS_warning("ALSA", "%s", snd_strerror(written));
			break;
		}
	}

	return (written == (snd_pcm_sframes_t) samples);
}