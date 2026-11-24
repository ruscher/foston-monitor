# Limitações conhecidas e próximas fases

## Limites do hardware

- **Multitouch:** um ponto só ([11](11-touchscreen-analysis.md)). Botões simultâneos vêm pelas
  travas ou pelo controle Bluetooth.
- **CPU para vídeo:** 468 MHz, sem NEON. A TV mostrou o custo: H.264 Baseline 320×180 a 30 qps
  ocupa a CPU, e Main com quadros B não cabe
  ([../energia-e-desempenho/03](../energia-e-desempenho/03-tv-fluidez.md)).
  - O vídeo do jogo (fase 6) vai pedir ao Sunshine o fluxo mais leve (320×180 ou 400×240, 10–15
    qps).
  - **Limite do protocolo:** o Sunshine codifica H.264 **High** com quadros de referência
    variados; o decodificador do FOSTON vai precisar da escada de quadros da TV ou de um fluxo
    mais simples.
- **Bluetooth:** o MT6611 precisa do celular como mestre do enlace para ter vazão (corrigido nesta
  rodada). Para vídeo, o PAN não é recomendado: só controle.

## Pendente, por fase

| Fase | O que falta |
|---|---|
| 2 | Testar o pareamento e a lista de apps com alguém aprovando no PC |
| 3 | Medir o Somente Controle no PC (CPU, atraso, USB e PAN) |
| 4 | Combos, layouts (PlayStation, corrida, luta, FPS), analógico direito, ajuste de tamanho e transparência |
| 5 | Teste do DualShock 4 pelo Bluetooth ao mesmo tempo que o toque; jogador 2 |
| 6 | Vídeo: decodificar H.264 (FFmpeg da TV), RGB565 no framebuffer, medir qps e atraso |
| 7 | Som: Opus (falta compilar a libopus para ARM) em `/dev/dsp` |
| 8 | Teclado virtual com modificadores travados, mouse/touchpad, menu de cima, troca de monitor pelas entradas `NN - Monitor` |
| 9 | Estudo do digitalizador capacitivo (hardware, opcional) |
