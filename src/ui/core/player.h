/* player.h — motor de reprodução de MP3 do shell (libmpg123 + OSS).
 *
 * Uma thread decodifica e escreve em /dev/dsp; a interface consulta posição,
 * duração, tags, informações técnicas e o espectro calculado a partir das
 * amostras que estão sendo tocadas. Sem libmpg123 no build, player_available()
 * é falso e a interface diz isso. */
#ifndef BF_PLAYER_H
#define BF_PLAYER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PL_STOPPED, PL_PLAYING, PL_PAUSED } pl_state_t;

typedef struct {
    char title[128], artist[128], album[128], year[8], genre[48];
} pl_tags_t;

typedef struct {
    int rate, channels;          /* Hz, 1/2 */
    int bitrate_kbps;            /* atual (VBR: do quadro corrente) */
    bool vbr;
    int layer, version;          /* MPEG 1/2/2.5 = 0/1/2 (como na libmpg123) */
    long long file_bytes;
    bool audio_out;              /* false: /dev/dsp indisponível (simulador), toca em silêncio */
} pl_info_t;

#define PL_BANDS 24

bool       player_available(void);
bool       player_open(const char *path);      /* abre e começa a tocar */
void       player_toggle(void);                /* pausa/continua */
void       player_stop(void);
void       player_seek_ms(int ms);             /* posição absoluta */
int        player_pos_ms(void);
int        player_len_ms(void);                /* 0 = desconhecida */
pl_state_t player_state(void);
bool       player_take_finished(void);         /* a faixa acabou sozinha (lê e limpa) */
void       player_tags(pl_tags_t *t);
void       player_info(pl_info_t *i);
/* capa (ID3v2 APIC) da faixa atual: dados do arquivo de imagem (JPEG/PNG)
 * e seu tipo MIME; false se a faixa não tem imagem */
bool       player_cover(const uint8_t **data, size_t *len, const char **mime);
/* níveis 0..100 de PL_BANDS faixas logarítmicas (graves -> agudos), já
 * suavizados (subida rápida, queda lenta) */
void       player_spectrum(uint8_t out[PL_BANDS]);
#endif
