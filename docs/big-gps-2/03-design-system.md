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

- No alto, a **faixa verde** da manobra (seta, distância, via, a manobra seguinte) e, embaixo
  dela, o **guia de faixas** quando há dados.
- **Coluna à esquerda** (150 px):
  - tempo que falta (grande), distância, hora de chegada;
  - velocidade atual (só com GPS de verdade ou na simulação);
  - embaixo, visão geral (olho) e **Encerrar** em vermelho.
- À direita, os controles redondos: zoom, N/↑/3D, seguir, satélites e voz.
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
