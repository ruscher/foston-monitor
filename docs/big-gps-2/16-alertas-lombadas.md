# Lombadas: as do mapa e as suas

## Do mapa (OSM)

`traffic_calming=bump/hump/table/cushion/yes/speed_bump`: são 93.328 no Brasil, no pacote de
alertas (ver [15](15-alertas-radares.md)).
- No mapa: triângulo **amarelo** a partir do zoom 15.
- Na rota: "Lombada à frente" a max(120 m, 8 s), e o chip "Lombada · 80 m".
- Duplicatas a menos de 40 m contam uma vez.

**Os dados do OSM são incompletos.** O Big GPS não afirma que todas as lombadas estão no mapa.

## As suas (marcadas no aparelho)

- **Toque longo** no lugar e **"Lombada aqui"** no painel. Se já houver um aviso seu a até
  30 m, o botão vira **"Tirar aviso"**.
- Ficam em `$BIGFOSTON_DATA/biggps-avisos.txt`, uma linha "lat lon tipo" por aviso, até 200.
  São gravados com arquivo temporário e troca atômica.
- No mapa: triângulo **azul** a partir do zoom 14. Os do mapa são amarelos, para não parecerem
  dados oficiais.
- Na rota (`bga_route_points`): valem se estiverem a até 12 m da via. Entram na mesma fila em
  ordem, com a mesma voz, e o chip diz **"Lombada (sua) · 80 m"**.

Verificado:
- `tests/test_bgalert.c`: um aviso na via entra e outro a 40 m não, em ordem com os do mapa,
  marcado como do usuário.
- Simulador: capturas `42l` (botão no painel) e `42m` (triângulo azul ao lado da via);
  `biggps-avisos.txt` gravado.

Bug encontrado pelo compilador e corrigido antes de entrar: o texto do chip, num formato
condicional, passaria uma palavra no lugar da distância para os avisos do mapa.
