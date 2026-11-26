# Rotas alternativas (de verdade)

O Contraction Hierarchies dá a melhor rota, mas não alternativas. Sem trocar o algoritmo, o
roteador (`bgrouter.c`, `compute_alts`) procura alternativas **calculadas pelo próprio
roteador**: nada desenhado à mão, nada inventado.

1. A rota principal sai primeiro e aparece na hora.
2. Na mesma thread, com cancelamento se chegar outro pedido, ele escolhe **pontos de
   passagem** de cada lado da rota (na metade, a 1/3 e a 2/3). A distância lateral é 18 % da
   rota, entre 700 m e 12 km. Para cada um, calcula início → ponto → destino (duas consultas
   comuns).
3. Só fica a alternativa que:
   - leva no máximo **35 %** a mais de tempo;
   - repete no máximo **70 %** da principal (e 80 % de outra alternativa já aceita), medido
     por `bgn_shared_permille`, só com inteiros e tolerância de 15 m;
   - não faz "ir até o ponto e voltar" (150 m antes e depois do ponto a menos de 60 m).
4. No máximo 2 alternativas. Rotas com menos de 3 km e rotas com paradas não procuram.

## Na tela

- As alternativas aparecem em **azul-claro**, mais finas e por baixo da principal.
- O painel da rota ganha um botão por alternativa, com a diferença de tempo e a distância
  ("+4 min · 28 km").
- Tocar troca: a alternativa vira a rota (com os alertas recalculados), e a anterior vira o
  botão ("−4 min · 23 km").
- Ao iniciar a navegação, só a rota escolhida fica.

## Verificado

- `tests/test_bgnavlegs.c`: rota igual = 1000‰; paralela a 30 m = 0; metade em comum ≈ 500;
  ponto e rumo a 1 km.
- No simulador (`--screenshot`, mapa e rotas do DF), rota de 23,3 km até Taguatinga:
  - 1 alternativa de 6 tentativas em 8 ms (28 km, +4 min);
  - capturas `42b` (botão) e `42j` (depois de trocar).
- No FOSTON: ainda não (o aparelho estava desligado). Cada tentativa custa duas consultas.
  No aparelho uma consulta fria leva ~0,4 s, então as alternativas devem chegar alguns
  segundos depois da rota.
