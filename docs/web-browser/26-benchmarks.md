# Medições

Só números medidos. Aparelho: FOSTON FS-707DC com BigFoston OS 0.4.0 (shell, Bluetooth e
GPS rodando, 85 MB disponíveis antes do navegador). Ferramenta: `tools/bigfoston/web-probe.py`
(eventos, tela e `/proc/PID/status` pelo SSH).

## 23/11/2026 — motor no FOSTON (sem internet: páginas locais)

| Página | Carregar | VmRSS | VmHWM | MemAvailable depois |
|---|---|---|---|---|
| `about:blank` + página de erro | 0,30 s | 11,6 MB | 13,6 MB | 85,9 MB |
| página de teste (CSS, formulário, 60 itens) | 0,74 s | 12,1 MB | 14,1 MB | 86,6 MB |
| artigo da Wikipédia "GPS" salvo (1,19 MB de HTML) | 47 s* | 39,8 MB | 42,0 MB | 56,4 MB |

\* Sem internet. Linha do tempo: leitura do arquivo até 12 s e processamento até 47 s,
mas o perfil registrou **5,4 s de CPU**: o resto foi espera pelas folhas de estilo
externas (`https://pt.wikipedia.org/...`), cujo DNS esgotava o tempo com a rede
bloqueada. A medida com internet ainda está por fazer.

A memória **não volta** ao sair de uma página grande (39,8 → 40,0 MB na página seguinte):
o núcleo guarda a página no histórico e o `malloc` do musl não devolve. Por isso o shell
encerra o motor ao sair do app.

## Perfil de CPU no FOSTON (artigo da Wikipédia, 2686 amostras = 5,4 s)

| Onde | % |
|---|---|
| alocador do musl (`malloc_impl`, `alloc_slot`, `free`, `get_meta`, `mmap`/`munmap`) | ~19 |
| FreeType (texto) | 5,8 |
| `css_select_style` (seleção de CSS) | 4,9 |
| `clock_gettime` | 3,6 |
| eventos do DOM (`_dom_node_dispatch_event`) | 2,3 |

Otimização planejada (fase 8): alocador próprio no `bfweb` (o do musl é o maior custo).

## PC (referência, x86-64, mesmo motor)

| Página | Carregar | VmRSS |
|---|---|---|
| example.com (HTTPS) | 1,02 s | 26,9 MB |
| pt.wikipedia.org/wiki/GPS (HTTPS) | 7,25 s | 77,9 MB |
| a mesma, sem imagens | 1,14 s | 66,5 MB |
| expired.badssl.com | recusada (certificado expirado) | — |

## bfdl (PC)

`netsurf-all-3.11.tar.gz` (9,08 MB): inteiro e com pausa em 2,87 MB e retomada (Range +
If-Range com ETag) — SHA-512 idêntico ao do tarball conferido nos dois casos.
