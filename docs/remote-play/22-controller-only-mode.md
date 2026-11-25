# Modo Somente Controle

> **Removido em 24/11/2026, a pedido:** o app ficou só com a imagem. Este documento registra o que
> existiu.


O FOSTON vira o controle de um jogo que aparece no monitor ou na TV ligados ao computador.

## Como funciona

1. **Sessão normal do Sunshine.** O Sunshine não tem sessão sem vídeo e o Big Remote Play não
   tem agente de entrada. A entrada vai pelo canal de controle do próprio Sunshine (ENet
   cifrado com a chave da sessão), então não há serviço novo exposto na rede.
2. **Menor pedido de vídeo.** `START app 640 360 30 500 0`: 640×360, 30 qps, 500 kbit/s.
3. **Vídeo descartado.** Os quadros chegam e são descartados em `dr_submit`, sem decodificar
   (custo de CPU só da rede). O som também é descartado.
4. **Botões e eixos.** Vão com `LiSendMultiControllerEvent` (controle 1).

## Estado

- O motor e a tela estão prontos. O teste com o PC depende do pareamento (alguém aprovando o PIN
  no computador).
- Quando o host está ocupado com outra sessão, o app oferece "Encerrar a sessão atual", com
  confirmação, porque isso derruba o outro aparelho.

## A medir (quando pareado)

- CPU e rede do FOSTON com o vídeo descartado;
- atraso do toque até o PC;
- o mesmo com o PAN Bluetooth no lugar do USB.
