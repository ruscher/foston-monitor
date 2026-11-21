# Apps do BigFoston OS (shell LVGL no FOSTON)

Estado em 2026-11-20. Todos testados no aparelho, salvo indicação.

| App | Estado | Notas |
|---|---|---|
| Multimídia | **FUNCIONANDO** | MP3 com libmpg123 (ponto fixo); capa, espectro real, progresso com avanço, aleatório/repetir, fila, informações, tela cheia — [audio.md](audio.md) |
| Arquivos | **FUNCIONANDO** | cartão SD, dados e sistema; texto, JPEG/SVG com zoom; MP3 abre no Multimídia; EPUB/PDF no Livros; copiar, recortar, renomear, apagar e nova pasta (só no SD e nos dados) |
| Terminal | **FUNCIONANDO** | `/bin/sh -l` num PTY com emulação VT100 (ash, top, vi); usuário `bigfoston` |
| Livros | **FUNCIONANDO** | EPUB, TXT, PDF (texto) e SVG — detalhes abaixo |
| GPS, TV, Bluetooth, FM | NÃO IMPLEMENTADO | dependem de hardware/firmware proprietário (ver [04-linux-port.md](04-linux-port.md)) |

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
