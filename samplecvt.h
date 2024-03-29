#ifndef SAMPLECVT_H
#define SAMPLECVT_H

#include <ogg/os_types.h>

/**
 * Converts decoded samples to signed 16-bit PCM.
 * @param pcm the raw samples
 * @param buffer a buffer for the decoded samples
 * @param samples the number of samples
 * @param channels the number of channels (1 for mono, 2 for stereo)
 */
#if TREMOR
void pack_samples(ogg_int32_t **pcm, char *buffer, int samples, int channels);
#else // libvorbis
void pack_samples(float **pcm, char *buffer, int samples, int channels);
#endif

#endif
