# Pino de destino arrastável

Tudo com toque simples, toque longo e arrasto (a tela é resistiva, de um ponto só):

1. **Toque longo** no mapa marca o destino. O pino vermelho aparece e o painel mostra as
   coordenadas, o aviso "Toque e segure o pino para movê-lo" e os perfis de rota.
2. **Toque longo no pino** (numa área de ±30 px em volta dele): o painel fecha, o pino cresce
   (125 %) e passa a seguir o dedo, com a ponta **36 px acima do dedo** para o dedo não cobrir
   o ponto.
3. Perto da borda **o mapa rola junto**, a 8 px por evento de toque. Isso só acontece depois
   que o pino já se moveu 16 px: pegar um pino que está perto da borda não faz o mapa correr.
   A ponta não sobe além de 50 px do topo, para o pino aparecer inteiro.
4. Ao **soltar**, o destino passa a ser o ponto da ponta (inclusive o último movimento do
   dedo) e o painel volta com as coordenadas novas e os perfis. A rota anterior é descartada
   e escolher o perfil recalcula. A rota não é recalculada durante o arrasto.
5. Durante a navegação o pino fica preso.

O destino guardado é sempre a coordenada que o usuário escolheu. O roteador encaixa a
partida e a chegada na via mais próxima ao calcular, sem mudar o ponto marcado.

## Verificado no FOSTON (25/11/2026, toques injetados com `tools/bigfoston/bftap -d X1 Y1 X2 Y2 HOLD`)

| Teste | Resultado |
|---|---|
| Marcar em (660, 150) | pino com a ponta em (660, ~148), painel com as coordenadas |
| Arrastar o pino para (430, 110) | o pino foi, as coordenadas mudaram (−15,68903, −47,82427) |
| 1ª versão: pegar o pino junto à borda de cima | **bug**: o mapa rolou ~690 m durante o toque longo e o pino sumiu. Corrigido (rolagem só depois de mover 16 px; topo limitado) |
| Depois da correção: dois arrastos seguidos, um encostando na borda | o mapa rola pouco, o pino continua visível |

Diferença restante: num arrasto sintético rápido (12 passos em 300 ms), a ponta ficou ~20 px
antes do ponto final, provavelmente pela suavização do touchscreen. Falta conferir com o dedo.
