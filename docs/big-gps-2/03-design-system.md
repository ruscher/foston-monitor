# Desenho da interface (800×480, toque resistivo)

As quatro imagens de referência não chegaram na conversa. O desenho segue as **descrições**
do pedido, sem copiar marca, ícones nem mapas da Apple. A principal referência é a imagem 2:
modo automotivo horizontal, com a lista de rotas na lateral e o mapa em destaque.

## Planejamento da rota

- **Painel à esquerda** (300 px de largura, do botão Voltar até a base). Nele:
  - o destino, com o X para fechar;
  - um **cartão por rota**: tempo grande, distância e hora de chegada, e o aviso de pedágio
    quando se sabe. A rota escolhida tem borda azul; tocar em outro cartão escolhe aquela rota;
  - **Iniciar** em verde, na largura toda;
  - Simular e + Parada.
- O mapa enquadra a rota na área livre **à direita** do painel (`fit_route`): a rota escolhida
  em azul forte e as alternativas em azul-claro.

## Navegação

- No alto, a **faixa da manobra** em material escuro (seta, distância, via, a manobra seguinte)
  e, embaixo dela, o **guia de faixas** quando há dados.
- **Coluna à esquerda** (150 px):
  - tempo que falta (grande), distância, hora de chegada;
  - velocidade atual (só com GPS de verdade ou na simulação);
  - embaixo, visão geral (olho) e **Encerrar** em vermelho.
- À direita, os controles redondos: zoom (+ e − numa cápsula só), N/↑/3D, seguir, satélites
  e voz.
- Avisos: radar ou lombada em amarelo ao lado da coluna; "SIMULAÇÃO" em vermelho à direita.

## Cores

| Uso | Dia / noite |
|---|---|
| Rota a percorrer | `#1a73e8` / `#4c8bf5`, contorno `#0b3d91` / `#0d2b5e` |
| Rota percorrida | `#9fbbe6` / `#5b6f8f` |
| Alternativas | `#8ab4f8` / `#6f8fc4` |
| Iniciar | `#1e8e3e` |
| Paradas | `#e8710a` |
| Alertas | amarelo `#f5b83d`, radar vermelho `#d93025` |

Capturas no simulador: `42b` (planejamento com alternativa), `42d` (navegação), `42j` (depois de
trocar de rota), `42k` (faixas).

## Revisão visual (26/11/2026, BigFoston OS 0.6.4)

As imagens de referência continuaram sem chegar: só fotos da tela do FOSTON aparecem nas
conversas do projeto. Esta revisão segue os princípios do Apple Maps sem copiar cores, ícones
nem marca:
- mapa calmo, para a rota ser o que mais aparece;
- informação legível num relance;
- materiais sóbrios.

**Paleta do mapa** (`src/ui/biggps/bgrender.c`): própria, no lugar da do OpenStreetMap Carto
(rodovias rosa, troncais salmão e secundárias amarelas competiam com a rota).

- **Noite**:
  - fundo `#1B1E24`;
  - ruas em cinza-azulado neutro, cada vez mais claras por categoria: serviço `#3B404B`,
    residencial `#474D5A`, terciária `#545B6A`, secundária `#666E80`, primária `#7D8597`;
  - troncais e rodovias num dourado suave (`#8C7D5C`, `#A08A5C`);
  - água azul fundo `#15334B`;
  - parques e matas verdes de pouca saturação.
- **Dia**:
  - fundo `#F4F2EE`;
  - ruas brancas com contorno claro;
  - primárias, troncais e rodovias em amarelo-laranja suave (`#FFF6DC`, `#FFE6A8`, `#FFD685`).
- Só a tabela de cores mudou: o custo de desenho é o mesmo. No FOSTON, seguir 16,3–16,8 qps e
  navegação 11,1–11,2 qps, iguais às medições anteriores.

**Faixa da manobra:**
- material escuro (`#1F232B`) com borda fina e sombra, no lugar do verde forte;
- distância em branco, via em cinza-claro e "Depois:" em cinza médio;
- o detalhe da seta em azul-claro (a cor da rota).

**Zoom:** + e − numa cápsula, com divisória. O fundo é da cápsula; o botão só realça ao tocar.

**Alertas na navegação:**
- navegando, o mapa mostra só os alertas **na rota** (semáforos, lombadas, radares, passagens de
  nível) e os seus. No centro de Brasília, os semáforos de todas as ruas enchiam a tela (visto
  no aparelho).
- fora da navegação, o mapa continua mostrando todos.

Capturas do simulador: `42`, `42b`, `42c`, `42d`, `42p`, `42q`.
