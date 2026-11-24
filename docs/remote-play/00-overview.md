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
| Motor `bfrp` (libgamestream + moonlight-common-c, ARMv6/musl, 194 KB) | **FUNCIONANDO NO APARELHO**: fala com o Sunshine 7.1.431 pelo cabo USB | [05](05-client-architecture.md) |
| App no grupo Jogos (pacote `.bfapp` com o motor dentro, 103 KB) | **FUNCIONANDO NO APARELHO** | [05](05-client-architecture.md) |
| Encontrar o computador pelo USB (192.168.7.1), estado e versão do Sunshine | **FUNCIONANDO NO APARELHO** | [05](05-client-architecture.md) |
| Pareamento por PIN (o FOSTON mostra, o computador aprova) | IMPLEMENTADO, NÃO TESTADO: precisa de alguém no PC para aprovar | [24](24-security.md) |
| Lista de jogos e apps do computador | IMPLEMENTADO, NÃO TESTADO (depende do pareamento) | [05](05-client-architecture.md) |
| Modo **Somente Controle** (sessão sem decodificar vídeo) | IMPLEMENTADO, NÃO TESTADO no PC | [22](22-controller-only-mode.md) |
| Controle virtual de toque único com botões travados | **FUNCIONANDO NO SIMULADOR**; no aparelho falta a sessão pareada | [14](14-virtual-button-engine.md) |
| Analógico virtual com zona morta e trava | **FUNCIONANDO NO SIMULADOR** | [14](14-virtual-button-engine.md) |
| Controle Bluetooth físico repassado ao PC (somado ao virtual) | IMPLEMENTADO, NÃO TESTADO | [14](14-virtual-button-engine.md) |
| Multitouch verdadeiro no painel original | **INVIÁVEL** | [11](11-touchscreen-analysis.md) |
| Vídeo, áudio, teclado, mouse, monitores, combos, layouts | PENDENTE (fases 6–8) | [31](31-known-limitations.md) |

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
