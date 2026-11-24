# Etapa 3 — Mapa de controles (SDL GameControllerDB)

## O formato

Cada controle tem um **GUID** de 32 dígitos hexadecimais, montado a partir do `EVIOCGID`
(barramento, fabricante, produto e versão, todos little-endian com zeros entre eles), e
uma linha no banco:

```
050000004c050000cc09000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,...,
  guide:b12,lefttrigger:a3,leftx:a0,...,x:b0,y:b3,platform:Linux,
```

O GUID acima é: Bluetooth (05), Sony (054c), DualShock 4 v2 (09cc), versão 0x0100.
Os campos dizem de onde vem cada controle:

| Campo | Significado |
|---|---|
| `a:b1` | o botão número 1 do aparelho é o A (o de baixo: ✕ no PlayStation) |
| `lefttrigger:a3` | o eixo número 3 é o gatilho esquerdo |
| `dpup:h0.1` | o direcional 0, máscara 1 (1 cima, 2 direita, 4 baixo, 8 esquerda) |
| `+a2`, `-a2` | só a metade positiva ou negativa do eixo |
| `a2~` | eixo invertido |
| `+leftx:b3` | um botão empurra metade de um eixo |

Os números seguem a ordem do SDL no Linux:
- **botões**: de `BTN_JOYSTICK` (0x120) até `KEY_MAX` e depois de 0 até `BTN_JOYSTICK`;
- **eixos**: de `ABS_X` em diante, pulando os direcionais `ABS_HAT*`;
- **direcionais**: os pares `ABS_HATnX/Y` presentes.

Esse é o código em `src/ui/core/padmap.c`, testado em `tests/test_padmap.c` (32 verificações).

## Ordem de busca (`resolve()` em `core/gamepad.c`)

1. **Banco do usuário**: `<dados>/controles/gamecontrollerdb.txt`, com os controles registrados
   no SC Controller.
2. **Bancos dos apps instalados**: `<dados>/apps/*/gamecontrollerdb.txt`, como o do SC
   Controller, com 746 modelos para Linux.
3. **Linhas embutidas** no shell: DualShock 4 (v1 e v2) e DualSense (e Edge) pelo
   Bluetooth sem o driver da Sony. Elas funcionam mesmo sem o app instalado.
4. **Padrão do Linux** pelos códigos: `BTN_SOUTH` é o A, `ABS_RX/RY` é o analógico direito,
   `ABS_Z/RZ` são os gatilhos, `ABS_HAT0` é o direcional, `BTN_DPAD_*`... Se o controle não
   tem `ABS_RX/RY`, `Z/RZ` vira o analógico direito, como em muitos controles HID genéricos.

A comparação de GUIDs segue o SDL:
- o CRC (bytes 2 e 3) não conta;
- primeiro vale o GUID exato;
- se não houver, vale uma linha com a versão zerada.

A versão importa: o mesmo DualShock com o driver da Sony (versão 0x8100) tem outro
mapa, e ele não é usado no lugar do 0x0100.

## Do mapa às ações

O estado padrão tem:
- 17 botões: A, B, X, Y, Back, Guia, Start, L3, R3, LB, RB, as 4 direções do direcional,
  Misc1 e touchpad;
- 6 eixos: os analógicos de -1000 a 1000 e os gatilhos de 0 a 1000.

O `gamepad.c` compara o estado novo com o anterior e age:
- **analógico esquerdo**: cursor;
- **A**: toca;
- **B**: Esc;
- **Y**: Enter;
- **X**: espaço;
- **LB/RB**: página acima e abaixo;
- **gatilhos**: volume, ligando acima de 60% e desligando abaixo de 30%;
- **Guia**: início;
- **Start**: tocar ou pausar;
- **direcional**: setas;
- **analógico direito**: rolar.

O mesmo código serve a qualquer controle do banco: não há mais regra fixa para a Sony.
