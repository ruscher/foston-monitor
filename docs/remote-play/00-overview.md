# Big Remote Play no FOSTON — visão geral

Estado em 24/11/2026, branch `feat/bigfoston-web`. App do grupo **Jogos**: o FOSTON como cliente do
Sunshine do computador, o mesmo do Big Remote Play do BigLinux.

Estados usados aqui:
- **FUNCIONANDO NO APARELHO** (testado no FS-707DC)
- **FUNCIONANDO NO SIMULADOR**
- **IMPLEMENTADO, NÃO TESTADO**
- **PENDENTE**
- **INVIÁVEL**

| Recurso | Estado | Documento |
|---|---|---|
| Auditoria do Big Remote Play e do Moonlight | feita | [01](01-repository-audit.md) |
| Motor `bfrp` (libgamestream + moonlight-common-c + H.264 da TV, ARMv6/musl, 1,39 MB) | **FUNCIONANDO NO APARELHO**: fala com o Sunshine 7.1.431 pelo cabo USB | [05](05-client-architecture.md) |
| App no grupo Jogos (pacote `.bfapp` com o motor dentro, 631 KB) | **FUNCIONANDO NO APARELHO** | [05](05-client-architecture.md) |
| Encontrar o computador pelo USB (192.168.7.1), estado e versão do Sunshine | **FUNCIONANDO NO APARELHO** | [05](05-client-architecture.md) |
| Pareamento por PIN (o FOSTON mostra, o computador aprova) | **FUNCIONANDO NO APARELHO** (depois da correção do endereço na pilha) | [24](24-security.md) |
| Lista de jogos e apps do computador | **FUNCIONANDO NO APARELHO**: Desktop Low, Desktop, 01/02/03 - Monitor | [05](05-client-architecture.md) |
| **Com imagem**: o vídeo do computador no FOSTON, qualidade Alta/Média/Baixa | **FUNCIONANDO NO APARELHO**: Baixa ~19 qps em tela cheia, 15 na janela; Alta ~5; 0 erros | [06](06-video-decoding.md) |
| Tela cheia com a barra que sobe ao tocar | **FUNCIONANDO NO APARELHO** | [06](06-video-decoding.md) |
| Mouse pela imagem (toque = ponteiro e clique, botão direito, arrastar, rolar) | **FUNCIONANDO NO APARELHO** (ponteiro e clique conferidos na imagem) | [19](19-mouse-touchpad.md) |
| Imagem sem faixas pretas (corte automático + cobrir sem esticar) e lupa −/+ (100–400 %) | **FUNCIONANDO NO APARELHO** | [06](06-video-decoding.md) |
| Som do computador (Opus em ponto fixo, mono 24 kHz, 15 % de CPU) | **FUNCIONANDO NO APARELHO** (placa tocando; ouvir é com você) | [10](10-audio.md) |
| Modo performance durante a sessão ("Ligar sozinho na imagem do Big Remote Play") | **FUNCIONANDO NO APARELHO**: GPS e tarefas pausados, interface com prioridade | — |
| Controle Bluetooth físico repassado ao PC | IMPLEMENTADO, NÃO TESTADO | — |
| Controle na tela e "Somente controle" | **REMOVIDOS** a pedido (24/11/2026); o histórico está em [14](14-virtual-button-engine.md) e [22](22-controller-only-mode.md) | — |
| Multitouch verdadeiro no painel original | **INVIÁVEL** | [11](11-touchscreen-analysis.md) |
| Áudio, teclado, troca de monitor no app, combos, layouts | PENDENTE | [31](31-known-limitations.md) |

## Telas

Capturas do roteiro (`core/screenshot.c`):
- `40-brp-inicio`: o computador, o estado e Parear;
- `40b-brp-pin`: o PIN para aprovar no computador;
- `40c-brp-apps`: pareado, com a lista de jogos;
- `40d-brp-controle`: a sessão com o controle na tela;
- `40e-brp-travado`: direita, A e o analógico travados ao mesmo tempo.

No aparelho, o app abre pelo grupo Jogos e mostra "Sunshine 7.1.431 · ainda não pareado · em uso
por outra sessão".

## Como instalar

```
tools/bigfoston/build-remoteplay.sh                       # motor + pacote
python3 tools/bigfoston/bfapp.py install pkg/apps/big-remote-play.bfapp
```

No computador: o Big Remote Play com o compartilhamento ligado (Sunshine).
