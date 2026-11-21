# biggps — motor do Big GPS

Biblioteca em C, sem LVGL e sem ponto flutuante por vértice, usada pela tela do mapa
(`screens/scr_map.c`) e pela ferramenta `bgview` (PC e aparelho).

| Arquivo | Função |
|---|---|
| `bgmap_format.h` | formato do pacote de mapa `.bgm` (comum ao construtor do PC) |
| `bgkinds.h` | zoom mínimo de cada tipo de feição (comum ao construtor) |
| `bgmap.c/.h` | leitura do pacote, índice, decodificação e cache de tiles |
| `raster.c/.h` | rasterizador inteiro RGB565 (polígonos, linhas grossas, tracejado) |
| `bgrender.c/.h` | estilos dia/noite, camadas, rotação, rótulos, conversões de coordenadas |
| `bgview.c` | renderiza uma vista em PPM, mede quadros e faz perfil (`-p`) |

```sh
make            # bgview para o PC (build/host)
make CROSS=1    # bgview estático para o aparelho (build/arm)
```

Documentação: [docs/big-gps](../../../docs/big-gps/00-visao-geral.md).
