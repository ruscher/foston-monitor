# Bluetooth integrado ao BigFoston OS: visão geral

Branch `feat/bigfoston-bluetooth-integration` (a partir da 0.3.0). Estados: **FUNCIONANDO NO
HARDWARE**, PARCIAL, NÃO IMPLEMENTADO, BLOQUEADO POR DEPENDÊNCIA.

| Recurso | Estado | Documento |
|---|---|---|
| Pilha (MT6611 + BlueZ + BlueALSA), firmware oficial com bibliotecas compartilhadas | **FUNCIONANDO NO HARDWARE** (instalado na memória interna em 22/11/2026) | [03](03-bluez-stack.md) |
| Pareamento com código na tela, app Bluetooth | **FUNCIONANDO NO HARDWARE** (POCO X4 pareado pela tela) | [../hardware-enablement/08](../hardware-enablement/08-bluetooth-profiles.md) |
| Conexão iniciada pelo FOSTON e reconexão no boot | **FUNCIONANDO NO HARDWARE** (5/5 depois da correção da troca de papel) | [18](18-reconnection.md) |
| Conexão iniciada pelo celular | NÃO TESTADO depois da correção | [18](18-reconnection.md) |
| Música do celular na Multimídia (A2DP) | **FUNCIONANDO NO HARDWARE**: 41,5 KB/s, 0 cortes em 60 s (depois da correção do papel no enlace) | [05](05-a2dp-multimedia.md) |
| Título, artista, álbum, duração, posição, controles (AVRCP) | **FUNCIONANDO NO HARDWARE** | [06](06-avrcp-metadata.md) |
| Aleatório/repetir pelo FOSTON | PARCIAL: mostra o estado; o app de teste recusa a mudança (aviso na tela) | [06](06-avrcp-metadata.md) |
| Capa do álbum (AVRCP BIP) | BLOQUEADO: exige o obexd (+~1 MB), sem espaço na memória interna | [25](25-known-limitations.md) |
| Espectro com o áudio do celular | FUNCIONANDO NO SIMULADOR; no aparelho o anel anda a 44,1 kHz | [05](05-a2dp-multimedia.md) |
| Voz do Big GPS por cima da música do celular | PARCIAL: mecanismo testado (o PCM solta e volta); sem teste com navegação real | [05](05-a2dp-multimedia.md) |
| Internet pelo celular (PAN/BNEP, DHCP, DNS) | **FUNCIONANDO NO HARDWARE** (POCO X4, 24/11/2026) | [../energia-e-desempenho/04](../energia-e-desempenho/04-internet-pelo-celular.md) |
| Teclado e controle de jogo (HID) | **FUNCIONANDO NO HARDWARE** | [../jogos-e-pacotes/07](../jogos-e-pacotes/07-latencia-e-modo-performance.md) |
| Pareamento iniciado pelo celular | PARCIAL: chaves gravadas no cartão na hora; captura HCI em análise | [../energia-e-desempenho/05](../energia-e-desempenho/05-pareamento-celular.md) |
| Telefone (HFP/PBAP), microfone | NÃO IMPLEMENTADO (próximas fases) | [25](25-known-limitations.md) |

Ferramentas: `bfsnoop` (captura HCI em btsnoop, `btmon -r` no PC), `bfhci` (estado, `lp`),
`tools/bigfoston/bf-audio-profiler` (UART, cortes do PCM, CPU por processo),
`tools/bigfoston/bftap` (toque injetado para testar a interface).
