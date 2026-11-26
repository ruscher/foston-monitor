# Paradas intermediárias

- No painel da rota, **"+ Parada"**: aparece o aviso laranja "Toque e segure onde fica a parada",
  e o próximo toque longo no mapa marca a parada e recalcula.
- Até **3 paradas**, na ordem em que foram marcadas, com círculos laranja numerados no mapa
  (projetados também na vista 3D).
- **"Tirar paradas"** volta à rota direta.
- Mover o pino do destino mantém as paradas. Escolher outro destino (busca ou toque longo) as
  apaga.
- Na navegação:
  - cada parada é uma manobra "Você chegou à parada", com o ícone de chegada e a voz Faber;
  - ao recalcular por sair da rota, as paradas já alcançadas saem da lista.

## Como (sem mexer no Contraction Hierarchies)

O roteador (`bgrouter.c`) faz **uma consulta comum por trecho**: início → parada 1 → … →
destino. Cada trecho vira um `bgn_route_t`, e `bgn_append` (`bgnav.c`) os emenda:
- pontos, distâncias e tempos continuam do trecho anterior;
- a chegada do trecho anterior vira `BGN_WAYPOINT`;
- a saída do trecho seguinte some;
- se os dois trechos não se encostam (o encaixe na via pode cair alguns metros adiante), essa
  distância entra no total.

## Ainda não

- Reordenar e arrastar paradas: hoje é "tirar todas" e marcar de novo.
- Rotas alternativas: o Contraction Hierarchies não dá alternativas de graça (exigiria a
  técnica de via-nós ou de penalidades com buscas extras). Não é feito por enquanto, e nada é
  inventado no lugar.

## Verificado

- `tests/test_bgnavlegs.c`:
  - emenda com o ponto repetido;
  - emenda com folga de 10 m;
  - manobras e distâncias;
  - texto e voz da parada.
- No FOSTON, com toques injetados e GPS falso na hora atual:
  - rota de 2,6 km / 4 min;
  - "+ Parada" e toque longo: parada "1" no mapa e rota de 2,7 km / 5 min passando por ela,
    calculada em 417 ms;
  - o botão "Tirar paradas" aparece.
- Voz: "Você chegou à parada" no pacote de frases fixas (129 frases). A voz agora procura em
  todos os pacotes `.bgv`, o maior primeiro: dá para acrescentar frases sem refazer o pacote
  regional.
