# Testes do transmissor FM

## Feitos no FOSTON (22/11/2026)

| Teste | Resultado |
|---|---|
| `bffm status` (só leitura) antes de tudo | GPIO4 = 0, PWM3 desligada, clock do I²C desligado, PWM com 1785/892 (estado do CE) |
| `bffm on 88.1` | `ok 88.1`: PW_OK = 1 na primeira leitura do reg 0x0F |
| frequência lida de volta | 88,1 MHz (reg0 = 0x71, reg1 = 0xC3) |
| `bffm dump` | 32 registradores lidos sem erro de I²C |
| `bffm saida ambos` / `fm` | estado `speaker_off` 0 / 1 |
| `bffm off` | GPIO4 = 0, PWM3 = 0, clock do I²C = 0; alto-falante devolvido |
| ligar e desligar repetidas vezes | sem erro |
| app: Transmitir | em ~0,5 s "FM ATIVO — 88,1 MHz", indicador na barra; `bffm status`: pw_ok=1, 88,1 |
| app: Transmitir de novo | desligado, estado do boot |
| boot: `S44bffm start` | transmissor desligado |

## Com um rádio externo (22/11/2026, 88,1 MHz)

| Teste | Resultado |
|---|---|
| Música do Multimídia (estéreo, OSS) | ouvida no rádio |
| WAV mono, estéreo, só esquerdo e só direito (`aplay`, 32 kHz) | os 4 ouvidos no rádio e no alto-falante (modo *FOSTON + rádio FM*) |
| Teste de som (`bffm tom`, mono, OSS) | ouvido no rádio |
| TV ao vivo (`bftvd`, 32 kHz mono) | ouvida no rádio |
| Som de toque (`fx.c`, 22 050 Hz mono) | ouvido no rádio |
| Voz do Big GPS | não falava em nenhuma saída: o `espeak-ng` do sistema dependia de uma `libespeak-ng.so.1` não instalada; agora é estático e a frase de teste tocou |

Conclusão: o KT0801A transmite **tudo o que sai pelo DAC**, em qualquer formato, como deduzido do CE.
A voz das ligações não passa pelo DAC da ALSA (vai pelo DSP): falta testar.

## Falta

- [ ] Voz do Big GPS dentro do app, com o FM ligado.
- [ ] Voz de uma ligação (Telefone) no rádio.
- [ ] Modo *Só no rádio FM*: alto-falante mudo, som no rádio.
- [ ] Troca de frequência com o FM ligado (o rádio acompanha).
- [ ] Transmissão continuando com o app fechado (Multimídia, TV, GPS).
- [ ] Teste prolongado (30 min) e temporizador.



## Simulador

`make screenshots`: telas 44d/44f/44g/44h e 22 (estado de demonstração em
`BIGFOSTON_FM_STATUS`; o simulador não tem o `bffm`).
