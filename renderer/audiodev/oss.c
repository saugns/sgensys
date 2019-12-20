/* saugns: OSS audio output support.
 * Copyright (c) 2011-2014, 2017-2019 Joel K. Pettersson
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

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#if defined(__OpenBSD__) || defined(__NetBSD__)
# include <soundcard.h>
# define OSS_NAME_OUT "/dev/sound"
#else
# include <sys/soundcard.h>
# define OSS_NAME_OUT "/dev/dsp"
#endif

/*
 * \return instance or NULL on failure
 */
static inline SAU_AudioDev *open_oss(const char *restrict name, int mode,
		uint16_t channels, uint32_t *restrict srate) {
	const char *err_name = NULL;
	int tmp, fd;

	if ((fd = open(name, mode, 0)) == -1) {
		err_name = name;
		goto ERROR;
	}

	tmp = AFMT_S16_NE;
	if (ioctl(fd, SNDCTL_DSP_SETFMT, &tmp) == -1) {
		err_name = "SNDCTL_DSP_SETFMT";
		goto ERROR;
	}
	if (tmp != AFMT_S16_NE) {
		SAU_error("OSS", "16-bit signed integer native endian format unsupported");
		goto ERROR;
	}

	tmp = channels;
	if (ioctl(fd, SNDCTL_DSP_CHANNELS, &tmp) == -1) {
		err_name = "SNDCTL_DSP_CHANNELS";
		goto ERROR;
	}
	if (tmp != channels) {
		SAU_error("OSS", "%d channels unsupported",
			channels);
		goto ERROR;
	}

	tmp = *srate;
	if (ioctl(fd, SNDCTL_DSP_SPEED, &tmp) == -1) {
		err_name = "SNDCTL_DSP_SPEED";
		goto ERROR;
	}
	if ((uint32_t) tmp != *srate) {
		SAU_warning("OSS", "sample rate %d unsupported, using %d",
			*srate, tmp);
		*srate = tmp;
	}

	SAU_AudioDev *o = malloc(sizeof(SAU_AudioDev));
	o->ref.fd = fd;
	o->type = TYPE_OSS;
	o->channels = channels;
	o->srate = *srate;
	return o;

ERROR:
	if (err_name)
		SAU_error("OSS", "%s: %s", err_name, strerror(errno));
	if (fd != -1)
		close(fd);
	SAU_error("OSS", "configuration for device \"%s\" failed",
		name);
	return NULL;
}

/*
 * Destroy instance. Close OSS device,
 * ending playback in the process.
 */
static inline void close_oss(SAU_AudioDev *restrict o) {
	close(o->ref.fd);
	free(o);
}

/*
 * Write audio data.
 *
 * \return true if write sucessful, otherwise false
 */
static inline bool oss_write(SAU_AudioDev *restrict o,
		const int16_t *restrict buf, uint32_t samples) {
	size_t length = samples * o->channels * SOUND_BYTES;
	size_t written = write(o->ref.fd, buf, length);

	return (written == length);
}