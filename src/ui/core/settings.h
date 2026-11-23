/* settings.h — ajustes persistidos em $BIGFOSTON_DATA/settings.conf (chave=valor). */
#ifndef BF_SETTINGS_H
#define BF_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include "i18n.h"

typedef struct {
    bf_lang_t lang;
    int  mode;          /* BF_MODE_*: escuro (padrão), claro ou automático */
    int  palette;       /* tema de cores (core/theme.h, BF_PAL_*) */
    int  wallpaper;     /* BF_WALL_*: liso, degradê, degradê com o símbolo, imagem */
    char wall_image[256]; /* BF_WALL_IMAGE: arquivo escolhido na Galeria */
    bool anim;          /* animações de transição */
    bool sounds;        /* sons de toque */
    bool boot_sound;    /* som ao iniciar a interface */
    bool notify;        /* avisos na tela ao conectar dispositivos (core/devwatch.c) */
    int  dim_min;       /* escurecer a tela após N min sem toque (0 = nunca) */
    char tz[24];        /* fuso horário: id estável da tabela de core/clock.c */
    bool dst;           /* horário de verão manual (+1 h) nos fusos sem regra */
    bool auto_time;     /* atualizar a hora automaticamente (NTP) */
    bool gps_time;      /* acertar a hora pelo GPS quando ele tiver posição */
    char ntp[160];      /* servidores NTP separados por espaço (host ou host:porta) */
    int  home_mode;     /* BF_HOME_*: grade, painel ou quiosque */
    unsigned home_apps; /* bits da tabela de apps da Início (scr_home.c, só cresce no fim):
                           atalhos do Painel e apps visíveis no Quiosque */
    int  backlight;     /* 5..100 % */
    int  volume;        /* 0..100 % */
    bool show_date;     /* data na barra superior */
    bool clock_24h;
    int  music_repeat;  /* 0 desligado, 1 todas, 2 uma */
    bool music_shuffle;
    int  music_vis;     /* tela cheia: espectro 0 barras, 1 espelho, 2 LED, 3 linha */
    int  music_color;   /* 0 coral, 1 tema, 2 arco-íris, 3 fogo, 4 gelo */
    int  music_bg;      /* 0 cor do tema, 1 cor da capa, 2 papel de parede */
    bool music_light;   /* tela cheia em modo claro */
    bool gallery_sort;  /* Galeria: false recentes primeiro, true por nome */
    int  book_font;     /* Livros: 0..2 tamanho da letra */
    int  book_space;    /* 0..2 espaçamento entre linhas */
    int  book_margin;   /* 0..2 margens */
    int  book_theme;    /* 0 escuro, 1 sépia, 2 claro */
    bool nav_voice;     /* Big GPS: instruções faladas */
    int  map_detail;    /* Big GPS: BF_DETAIL_* (Ajustes > Desempenho) */
    bool map_avoid_toll; /* Big GPS: rotas de carro e moto sem pedágio quando houver o arquivo */
} bf_settings_t;

enum { BF_MODE_DARK, BF_MODE_LIGHT, BF_MODE_AUTO, BF_MODE__N };
/* detalhe do mapa: automático escolhe pela situação e pelo tempo de cada quadro */
enum { BF_DETAIL_AUTO, BF_DETAIL_ECONOMIC, BF_DETAIL_BALANCED, BF_DETAIL_DETAILED, BF_DETAIL__N };
enum { BF_HOME_GRID, BF_HOME_PANEL, BF_HOME_KIOSK, BF_HOME_GLASS, BF_HOME__N };   /* só cresce no fim */
/* Multimídia, Livro, GPS, Arquivos, Monitor, Ajustes */
#define BF_HOME_APPS_DEFAULT ((1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 9) | (1u << 10))
enum { BF_WALL_PLAIN, BF_WALL_GRADIENT, BF_WALL_LOGO, BF_WALL_IMAGE, BF_WALL__N };
/* papel de parede padrão (data/wallpapers, livre para redistribuir); no simulador, relativo
 * a src/ui. Sem o arquivo, o tema usa o degradê com o símbolo. */
#ifdef BIGFOSTON_TARGET
#define BF_WALL_DEFAULT_IMAGE "/usr/share/bigfoston/wallpapers/ondas-digitais.jpg"
#else
#define BF_WALL_DEFAULT_IMAGE "data/wallpapers/ondas-digitais.jpg"
#endif
#define BF_DIM_CHOICES { 0, 1, 2, 5, 10 }
/* o PC pela rede USB (se servir NTP), depois o NTP.br e o pool */
#define BF_NTP_DEFAULT "192.168.7.1 a.st1.ntp.br pool.ntp.org"

bf_settings_t *settings(void);
void settings_defaults(bf_settings_t *s);
void settings_load(void);
bool settings_save(void);

/* Pasta de dados: $BIGFOSTON_DATA ou o padrão do build
 * (/data/bigfoston no alvo, ./data no simulador). Criada se não existir. */
void        data_dir_init(const char *override);
const char *data_dir(void);
/* true quando a pasta de dados está na RAM (tmpfs/initramfs): os ajustes se
 * perdem ao desligar. */
bool        data_dir_volatile(void);
/* Monta "<data_dir>/<name>" em out. */
void        data_path(char *out, size_t n, const char *name);

#endif
