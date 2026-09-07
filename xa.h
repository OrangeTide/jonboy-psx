#ifndef XA_H
#define XA_H

#include <stdint.h>

// CD-ROM XA-ADPCM streaming audio decoder.
//
// A Mode2 XA audio sector carries 2304 bytes of ADPCM sound-group data after the
// 24-byte header/subheader. This module decodes one such sector to 16-bit PCM,
// resamples it from the stream rate (37800 or 18900 Hz) to the 44100 Hz output
// rate, and hands it to the SPU CD-audio input (spu_cd_audio). The ADPCM filter
// history and the resampler phase persist across sectors, so a stream stays
// continuous; xa_reset clears that state when a new stream starts.

// Decode the 2304-byte sound-group area of one XA audio sector and feed the
// resampled PCM to the SPU. `groups` points at the sound-group data (raw sector
// byte 24). `coding` is the subheader coding byte (raw sector byte 19): stereo in
// bits 0-1, sample rate in bits 2-3 (0 = 37800, 1 = 18900), bit depth in bits 4-5
// (0 = 4-bit, 1 = 8-bit).
void xa_decode_sector(const uint8_t *groups, uint8_t coding);

// Clear the decoder and resampler state. Call when a stream starts or the drive
// is reset so filter history and resampler phase do not carry across streams.
void xa_reset(void);

#endif
