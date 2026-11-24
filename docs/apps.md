# Apps do BigFoston OS (shell LVGL no FOSTON)

Apps de fora do sistema (pacotes `.bfapp`, grupos como Jogos): [jogos-e-pacotes](jogos-e-pacotes/00-visao-geral.md).

Estado em 2026-11-21. Todos testados no aparelho, salvo indicação.

| App | Estado | Notas |
|---|---|---|
| Multimídia | **FUNCIONANDO** | MP3 com libmpg123 (ponto fixo); capa, espectro real, progresso com avanço, aleatório/repetir, fila com rolagem, informações, volume com mudo; tela cheia com capa grande, álbum/ano/gênero, volume próprio e aparência configurável (espectro, cores, fundo, claro/escuro) — [audio.md](audio.md) |
| Arquivos | **FUNCIONANDO** | cartão SD, dados e sistema, listas com rolagem; texto; JPEG, PNG, GIF, BMP e SVG com zoom; MP3 abre no Multimídia; EPUB/PDF no Livros; copiar, recortar, renomear, apagar e nova pasta (só no SD e nos dados) |
| Terminal | **FUNCIONANDO** | `/bin/sh -l` num PTY com emulação VT100 (ash, top, vi); usuário `bigfoston` |
| Livros | **FUNCIONANDO** | EPUB, TXT, PDF (texto) e SVG — detalhes abaixo |
| Galeria | **FUNCIONANDO** | JPEG, PNG, GIF (animado no visualizador), BMP e SVG — detalhes abaixo |
| GPS (Big GPS) | **FUNCIONANDO** | mapa offline em tela cheia, destino por toque longo ou busca, rotas para carro/moto/bicicleta/a pé, navegação com voz, favoritos e recentes — [big-gps](big-gps/00-visao-geral.md); estado do receptor no botão de satélites ([gps.md](gps.md)) |
| SC Controller (pacote, grupo Jogos) | **FUNCIONANDO** | teste ao vivo, mapa do SDL GameControllerDB e registro de controles; instalado por `.bfapp` — [jogos-e-pacotes](jogos-e-pacotes/00-visao-geral.md) |
| TV, Bluetooth, FM | NÃO IMPLEMENTADO | dependem de hardware/firmware proprietário (ver [04-linux-port.md](04-linux-port.md)) |

## Arquivos: gravação

Segurar um item abre Copiar, Recortar, Renomear e Apagar (Apagar confirma; pastas
inteiras); nas pastas graváveis a barra tem Nova pasta e Colar. Só grava no cartão SD
liberado para gravação e na pasta de dados — o sistema (`/`) continua só leitura.
Cópias, movimentações e exclusões rodam numa thread (`core/fileops.c`) com barra de
progresso e Cancelar; nada fica pela metade, conflito de nome vira "nome (2)", nomes
inválidos no FAT são recusados e o fim de cada operação faz `sync`. Testado no aparelho:
22 verificações direto no cartão (inclusive cancelar uma cópia de 80 MB).

## Livros

- **Biblioteca:** `*.epub`, `*.pdf`, `*.txt` e `*.svg` em `/mnt/sd` e em `<dados>/books`, em grade de capas. A capa JPEG do EPUB é carregada aos poucos; os outros recebem uma capa gerada.
- **Leitor:**
  - páginas medidas com a fonte real;
  - paginação completa em segundo plano, com cache por livro e layout em `<dados>/bookcache`;
  - sumário, marcadores (`bookmarks.conf`), busca;
  - barra deslizante de páginas;
  - A−/A+, espaçamento, margens, temas escuro/sépia/claro;
  - gestos de deslizar.
- **PDF:** o texto é extraído (`core/pdf.c`) e paginado como livro. Não renderiza as páginas: um renderizador (MuPDF/Poppler) custaria vários MB, mais que o sistema inteiro. PDF só de imagem é recusado com explicação.
- **SVG:** nanosvg, rasterizado de novo a cada nível de zoom (no máximo 2048 px de lado).

Medido no FS-707DC (ARM1176 468 MHz, sem FPU):

| Operação | Tempo |
|---|---|
| Paginação | ~30 ms por página (EPUB de 19 KB, 41 páginas: 1,26 s), sem travar a tela |
| Renderização SVG (tigre de teste, 264×428) | 2,5 s; ampliado a 396×641: 3,1 s |

O nanosvg usa ponto flutuante emulado. SVGs complexos ficam lentos.

## Galeria

`src/ui/screens/scr_gallery.c` e `src/ui/core/imgload.c`. Procura imagens no cartão SD e na
pasta de dados (até 5 níveis de pasta, 600 imagens; o mesmo arquivo visto por dois caminhos,
como a pasta de dados montada do cartão, conta uma vez).

| Formato | Como |
|---|---|
| JPEG | TJpgDec do LVGL, reduzindo 1/2 ou 1/4 enquanto decodifica (o 1/8 sai corrompido); progressivo não é suportado |
| PNG | lodepng, até 6 Mpx; transparência misturada com o fundo |
| GIF | gifdec do LVGL: primeiro quadro na miniatura; animado no visualizador (até 1200 px e 2 MB) |
| BMP | 1, 4, 8, 16, 24 e 32 bits sem compressão |
| SVG | nanosvg |

As miniaturas (120×120) são feitas numa thread, só para a parte visível da grade, e ficam em
`<dados>/thumbs` quando o cartão aceita gravação: a segunda vez é imediata (as 8 imagens de
exemplo ficaram prontas em menos de 20 s no aparelho, na primeira vez). Toque abre o visualizador
(`src/ui/widgets/imgview.c`, também usado por Arquivos e Livros), no mesmo layout do leitor de
livros: a barra superior do sistema continua visível e um toque na foto mostra/esconde os
controles — em cima voltar, nome, zoom −, %, zoom +, Ajustar, girar ⟲ e ⟳; embaixo ◀, barra
entre as fotos com "N / M", ▶ e resolução · tamanho · tipo. Deslizar (sem zoom) troca de foto e
o Voltar do sistema fecha a foto antes de sair da tela. O giro é de 90° nos pixels já reduzidos;
GIF animado não tem zoom nem giro. Correções no LVGL 9.2 vendorizado (`gifdec.c`): o último código LZW que
completa o quadro era recusado (`>=` → `>`) e GIF87a era rejeitado.
