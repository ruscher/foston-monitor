/* audio.h — som do Big Remote Play (audio.c).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BFRP_AUDIO_H
#define BFRP_AUDIO_H
#include <Limelight.h>

int  audio_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION oc);   /* 0 ok, 1 sem alto-falante, -1 erro */
void audio_sample(char *data, int len);         /* um pacote Opus (NULL: perdido) */
void audio_mute(int on);
int  audio_ok(void);
void audio_stats(unsigned long *pkts, unsigned long *lost, unsigned long *underruns);
void audio_close(void);
#endif
