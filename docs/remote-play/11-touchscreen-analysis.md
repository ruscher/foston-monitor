# Touchscreen: por que não há multitouch no painel original

Fonte: [../touchscreen-driver.md](../touchscreen-driver.md) e o driver
`firmware/kernel/overlay/drivers/input/touchscreen/mt3351-ts.c`.

- **O painel:** resistivo de 4 fios, lido pelo bloco ADC em `0x80034000`, com quatro canais:
  placa horizontal (`0x10`), vertical (`0x50`), Z1 (`0x30`) e Z2 (`0x40`).
- **O que o ADC mede:** uma tensão por eixo. Com dois dedos, as duas placas encostam em dois
  lugares e a tensão lida é a de um ponto **entre** eles, que não é nenhum dos dedos.
  - As medidas de pressão (Z1/Z2) mudam quando há dois contatos, porque a resistência de contato
    cai. No máximo isso daria um **indício** de "mais de um dedo", nunca duas posições.
- **O que o driver informa:** um ponto (X, Y) e o toque, sem `ABS_MT_*`.

## Resultado

| Opção | Viável? |
|---|---|
| 1. Multitouch verdadeiro no painel original | **Não.** O hardware não informa duas posições |
| 2. Detecção limitada de gestos (Z1/Z2 indicando 2 contatos) | Talvez como indício, sem posição; **não implementado** e não usado como entrada |
| 3. Simulação por software | Não: uma posição média não pode virar dois botões |
| 4. Controle com estados persistentes | **Sim:** é o que o app faz ([14](14-virtual-button-engine.md)) |
| 5. Digitalizador capacitivo novo | Estudo de hardware, opcional ([32](32-physical-multitouch-upgrade.md), pendente) |
| Controle Bluetooth | **Sim:** vários botões de verdade ao mesmo tempo |

O app **não** trata posições médias ou leituras instáveis como dois contatos.
