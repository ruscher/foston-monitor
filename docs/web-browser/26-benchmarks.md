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

## 23/11/2026 — modo completo (Chrome 157 no PC, FOSTON pela rede USB, tema Glass 800×380)

| Teste | Resultado |
|---|---|
| g1.globo.com: comando → página na tela | ~8 s |
| página parada (biglinux.com.br) | shell 7 % de CPU |
| rolando o g1 | shell 14 % |
| vídeo do YouTube tocando | shell 58 %, MemAvailable ~68 MB (o GPS seguiu rodando) |
| antes do controle de fluxo (página animada) | shell 68 % e comandos atrasados: corrigido com um quadro por vez (`Q`) e ~12 quadros/s |
| download de 9,08 MB pelo PC | 13 s, SHA-512 idêntico no cartão |

## Motor local no FOSTON, sem internet (firewall do PC bloqueando)

A página de erro explica a causa em português ("O nome do site não foi encontrado (DNS)...")
e o app faz um diagnóstico (DNS bloqueado × nada passa × site inexistente × site fora do
ar). Antes, a libcurl estava sem as mensagens de erro (`BR2_PACKAGE_LIBCURL_VERBOSE`) e
tudo virava "Error".

## 23/11/2026 — motor local no FOSTON com internet (PC compartilhando, ufw liberado)

`web-probe.py --ssh`, motor como root sem limite (as medidas nas condições do app, usuário
`bigfoston`, nice 10 e 88 MB, deram os mesmos tempos). Uma página após a outra no mesmo
processo: "CPU" e VmHWM são acumulados.

| Página | Carregar | CPU (acum.) | VmRSS | MemAvailable depois |
|---|---|---|---|---|
| example.com | 3,4 s | 2,3 s | 14,0 MB | 79,5 MB |
| biglinux.com.br (→ www → /en/ por `<meta refresh>`) | 20,8 s | 16,5 s | 30,3 MB | 55,3 MB |
| pt.wikipedia.org/wiki/GPS (página de 43 700 px de altura) | 80,9 s | 69,1 s | 61,5 MB | 33,5 MB |
| lite.duckduckgo.com (busca) | 6,5 s | 74,2 s | 61,4 MB | 34,0 MB |
| g1.globo.com (fica vazio: depende de JavaScript) | 20,2 s | 90,0 s | 49,4 MB | 44,3 MB |
| youtube.com, motor novo (só a casca, sem vídeo) | 45,9 s | — | — | 62,7 MB |
| youtube.com de novo, mesmo motor | 5,1 s | — | — | 64,3 MB |

Isolada (motor novo), a Wikipédia leva 77 s: ~12 s baixando e ~65 s processando, com
52 s de CPU. A versão móvel (`pt.m.wikipedia.org`) dá o mesmo. Perfil dessa carga
(`BFWEB_PROF`, 5436 amostras de 10 ms):

| Onde | % |
|---|---|
| NetSurf (CSS, DOM, layout) | ~48 |
| alocador do musl (`malloc`, `free`, `mmap`/`munmap`...) | ~18 |
| OpenSSL: AES-GCM ~3,5 · troca de chaves (`bn_mul_mont`) 1,5 · certificados (ASN.1) ~2 | ~14 |
| `clock_gettime` (sem vDSO no ARMv6: cada chamada é uma syscall) | 2,7 |
| FreeType | 3,3 |

Nenhuma parte sozinha muda o quadro: trocar AES por ChaCha20 economizaria ~1 s, um
alocador mais rápido talvez 6–7 s. Para páginas pesadas o caminho é o modo completo.

## bfdl no FOSTON (internet própria)

`netsurf-all-3.11.tar.gz` (9,08 MB) direto para o cartão: pausado (SIGTERM) com 112 KB,
retomado e concluído em 14 s (~0,65 MB/s); SHA-512 idêntico ao do tarball conferido.

## 23/11/2026 — som no modo completo

Vídeo do YouTube tocando, estado do PCM (`/proc/asound/card0/pcm0p/sub0/status`) lido a
cada 0,1 s no FOSTON:

| Versão | Resultado |
|---|---|
| som pela mesma conexão da imagem, lido pela interface | 60 amostras: 40 `RUNNING`, 8 `XRUN`; intervalos de até 318 ms sem pacote de som (a interface desenhando o vídeo); shell 78 % |
| conexão própria do som, threads com nice −8 | 180 s seguidos em `RUNNING`, nenhum `XRUN`; 67 s com duas rolagens no meio, nenhum `XRUN`; shell ~61 % |

Taxa: 128 KB/s (PCM 32 kHz estéreo), só enquanto há som.
