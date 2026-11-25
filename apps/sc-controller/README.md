# sc-controller (pacote do BigFoston)

Pasta-fonte do pacote `sc-controller.bfapp`: o app SC Controller do grupo Jogos.

- `sc-controller.desktop`: `Categories=Game;...` põe o app em Jogos; `X-BigFoston-Screen` abre
  a tela nativa (`src/ui/screens/scr_scc.c`).
- `icon.svg`: a silhueta do ícone (o shell pinta de branco sobre a cor `#7a4cd9`).
- `gamecontrollerdb.txt`: as linhas de Linux do SDL GameControllerDB, vindas do
  [sc-controller](https://github.com/ruscher/sc-controller); licença zlib em
  `LICENSE-gamecontrollerdb.txt`. Para atualizar: `./update-db.sh`.

Montar e instalar: `python3 tools/bigfoston/bfapp.py build apps/sc-controller` e
`... install pkg/apps/sc-controller.bfapp`. Documentação:
[docs/jogos-e-pacotes](../../docs/jogos-e-pacotes/00-visao-geral.md).
