# Marcador da posição personalizável

**Tocar no marcador da posição** abre o painel "Seu marcador":
- **perfil:** Carro, Moto, Bicicleta, A pé;
- **modelo**, com prévia na cor escolhida;
- **cor** (8);
- **tamanho:** P 80 %, M 100 %, G 125 %;
- **Restaurar padrão**.

Tudo é salvo por perfil (`map_veh_N = modelo cor tamanho` no `settings.conf`). O marcador
mostrado é o do perfil da rota (carro se ainda não houver rota).

| Perfil | Modelos (o primeiro é o padrão) |
|---|---|
| Carro | seta, sedã, compacto, SUV, esportivo |
| Moto | seta, moto vista de cima, ponto |
| Bicicleta | seta, bicicleta vista de cima, ponto |
| A pé | ponto, pessoa vista de cima, seta |

Comportamento:
- **Seta:** seta no rumo quando anda, ponto quando parado.
- **Veículo:** sempre o veículo, no último rumo conhecido.
- **Posição incerta:** cinza translúcido (GPS perdido há mais de 3 s, ou sem fix).

## Desenho (`src/ui/biggps/bgvehicle.c`)

Cada modelo é um punhado de camadas (retângulos arredondados, elipses, polígonos) com papéis
de cor:
- corpo;
- contorno escuro, ou claro em veículo escuro;
- vidro;
- faróis;
- teto;
- rodas.

O desenho é feito **uma vez**, com 4×4 amostras por pixel, num bitmap 48×48 ARGB. Depois o
LVGL só gira e escala. Nada é redesenhado por quadro.

**Bug encontrado no aparelho e corrigido:** a 1ª versão calculava com `float`. Sem FPU, cada
bitmap levava segundos e o painel travava (os toques se acumulavam). Agora as camadas são
convertidas para inteiros (1/16 px) com caixa de rejeição rápida: as 5 prévias saem em
**107–189 ms** no FOSTON. No PC o resultado difere só em pixels de borda.

Outro cuidado: o painel é refeito com `lv_async_call` depois do toque. O botão tocado é
filho do painel que é apagado, e apagar o pai dentro do próprio evento não é seguro.

Contraste conferido nos fundos claro e escuro: a seta branca ganha borda escura, e as rodas
da bicicleta são cinza-médias para aparecerem à noite.

## Verificado

- No simulador: capturas `42g` a `42i` (`--screenshot` com `BIGFOSTON_BIGGPS`).
- No FOSTON, com toques injetados (`bftap`) e um GPS falso na hora atual:
  - abrir o painel, escolher sedã, vermelho e G, e fechar;
  - o `settings.conf` gravou `map_veh_0 = 2 1 2`;
  - o sedã vermelho grande aparece no rumo (62°).

## Também corrigido: abrir o mapa custava 433 ms

O pino do destino, a lupa da busca e os marcadores antigos eram desenhados com `float` em
`mk_build` a cada abertura do mapa: **433 ms** no FOSTON. Agora o pino e a lupa saem do mesmo
rasterizador em inteiros (`BGVEH_PIN`, `BGVEH_MAGNIFIER`), e os marcadores da posição de
`veh_sync`: **39 ms**, com a mesma aparência.
