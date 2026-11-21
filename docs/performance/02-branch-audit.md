# Auditoria das branches (21/11/2026)

| Branch | Situação |
|---|---|
| `main` (origin) | merge do PR #2; nenhum conteúdo além do que está em `bigfoston-linux` local |
| `bigfoston-linux` (origin) | 28 commits atrás da cópia local (Big GPS completo, botão de energia, kexec) |
| `big-foston-os` (origin) | 87 commits atrás; seu único commit próprio é um merge sem conteúdo novo |
| `bigfoston-linux` (local) | a versão mais nova; é o que roda no aparelho |

`git diff HEAD...origin/big-foston-os` e `...origin/main` vazios: nada a recuperar das
outras branches. Trabalho em `perf/bigfoston-extreme-optimization`, criada a partir da
local, sem merges nem push forçado.

O mapa de navegação é `src/ui/screens/scr_map.c` + `src/ui/biggps/` (motor próprio);
`scr_gps.c` é a tela de satélites. Nenhum segundo motor foi criado.
