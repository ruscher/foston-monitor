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

## Falta (depende de um rádio externo)

- [ ] **Ouvir o FOSTON num rádio FM** sintonizado na mesma frequência (música do Multimídia).
- [ ] Modo *Só no rádio FM*: alto-falante mudo, som no rádio.
- [ ] Modo *FOSTON + rádio FM*: som nos dois.
- [ ] Teste de som (bipes) no rádio.
- [ ] Troca de frequência com o FM ligado (o rádio acompanha).
- [ ] Transmissão continuando com o app fechado (Multimídia, TV, GPS).
- [ ] Teste prolongado (30 min) e temporizador.

Sem esses testes, "o áudio chega ao rádio" **não está comprovado**: o chip aceitar os comandos
não basta.

## Simulador

`make screenshots`: telas 44d/44f/44g/44h e 22 (estado de demonstração em
`BIGFOSTON_FM_STATUS`; o simulador não tem o `bffm`).
