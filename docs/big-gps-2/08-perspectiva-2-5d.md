# Vista inclinada (2.5D)

## Como é feita (`src/ui/biggps/bgpersp.c`)

Sem aceleração gráfica, a inclinação é um remapeamento por linha ("modo 7"):

1. O motor desenha um quadro de cima, como sempre, mas de uma **área maior**: 1168×634 px,
   1,93× a tela, mais funda à frente e mais larga no alto.
2. Cada linha da tela lê **uma linha da fonte**, escalada na horizontal em volta do centro.
   É uma câmera olhando o chão:
   - linha da tela `y = yh + Z0·(y0 − yh)/Z`;
   - coluna `x = cx + (sx − cx)·Z0/Z`.
   O carro fica na linha 350 (`PERSP_Y0`), com escala 1, igual ao 2D. O horizonte fica 800 px
   acima da tela (`PERSP_YH`) e nunca aparece.
3. Tabelas por linha (linha de origem, início e passo em 16.16) são calculadas uma vez. Por
   pixel é uma leitura e uma soma: **26,6 ms por quadro no FOSTON**. Uma cópia simples da tela
   custa 12,2 ms.

Entre os quadros do motor, o carro anda **deslocando a fonte** e projetando de novo. Isso só
acontece quando o deslocamento muda pelo menos 1 px, como no 2D.

Os nomes de ruas são compostos **na fonte**, antes de projetar: inclinam junto com o mapa,
continuam legíveis e não custam nada a mais por projeção. Radares, lombadas e semáforos também
vão na fonte. O marcador do carro e o pino do destino são projetados ponto a ponto
(`bgp_project`).

## Quando liga

- O botão de orientação passa por **N → ↑ (no rumo) → 3D**. A escolha é salva (`map_persp`) e o
  padrão é 3D.
- A vista só fica inclinada com o mapa **seguindo no rumo**. Arrastar o mapa volta ao 2D até
  tocar no botão de posição.
- Fica no 2D com o detalhe "Econômico" (Ajustes > Desempenho).
- **Recuo automático**: com 6 quadros seguidos do motor acima de 700 ms, volta ao 2D na sessão
  e o botão mostra ↑.
- `BIGGPS_PERSP=0` desliga, para comparar.

## Medido no FOSTON (25/11/2026, roteiro de bench, mesmo marcador)

| | 2D no rumo | Inclinada |
|---|---|---|
| Seguir | 18,6 quadros/s, CPU 49 % | 16,1 quadros/s, CPU 55 % |
| Navegação | 13,1 quadros/s, CPU 53 % | 11,7 quadros/s, CPU 64 % |
| Quadro do motor (navegação, p50) | 142 ms | 346 ms |
| Laço da interface p95 (navegação) | 97 ms | 136 ms |
| RSS | 16,8 MB | 20,8 MB (+2 fontes de 1,5 MB) |

A queda é moderada e fica acima da meta de 10 quadros/s úteis. A 1ª regra de recuo (média
acima de 500 ms) disparou num bench por causa de picos de leitura do cartão e tirava o 3D da
sessão inteira. Agora são 6 quadros seguidos acima de 700 ms.

## Limites

- O giro da câmera, como no 2D, vem em degraus a cada quadro do motor (que agora é mais lento).
- Os nomes no alto ficam mais estreitos (projetados). Apple Maps os mantém retos; aqui isso
  custaria compor os nomes de novo a cada projeção (+30–40 ms).
- Não há horizonte nem céu: a inclinação é moderada de propósito (custo e legibilidade).

Testes: `tests/test_bgpersp.c`:
- escala 1 na linha do carro;
- ordem das linhas;
- projetar e desprojetar;
- o remapeamento lê o pixel certo;
- deslocamentos grandes sem sair da fonte (ASan);
- inclinação exagerada recusada.
