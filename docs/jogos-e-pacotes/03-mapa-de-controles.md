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

## DualShock 4 pelo Bluetooth: o relatório 0x11 (24/11/2026)

O DualShock 4 de verdade conectava, mas **nenhum botão chegava** (0 eventos no
`/dev/input/event*`). O controle virtual dos testes mandava eventos prontos e não mostrou isso.

| Onde | O que a captura HCI (`bfsnoop` + `btmon`) mostrou |
|---|---|
| Bluetooth | pacotes de 79 bytes no canal de interrupção, ~4/s parado: `a1 11 c0 00 80 7f 80 7f 08 ...` |
| Kernel | o descritor HID que o BlueZ passa ao `uhid` (o do SDP) só declara botões e eixos no relatório **0x01**; o **0x11** (o que o controle manda pelo Bluetooth, com giroscópio e bateria) é "dados do fabricante" |
| Resultado | o `hid-generic` descarta o 0x11 inteiro |

O 0x11 traz o conteúdo do 0x01 depois de 2 bytes de cabeçalho (é a `dualshock4_input_report_bt`
do `hid-playstation`). Sem o driver da Sony no kernel, a correção é no **descritor**:

- o 0x11 passa a declarar, depois dos 2 bytes, os mesmos analógicos (X Y Z Rz), o direcional,
  os 14 botões, o contador e os gatilhos (Rx Ry) do 0x01; o resto continua do fabricante;
- o 0x01 fica sem campos, porque o `hid-input` **ignora um uso repetido** (o segundo relatório
  ficaria mudo, `HID_STAT_DUP_DETECTED`);
- o descritor continua com **431 bytes**: um par Push/Pop (`a4 b4`) completa o tamanho, e
  nenhum comprimento do registro SDP muda.

Os botões e eixos que o kernel cria são idênticos aos de antes (`0x130`–`0x13d`, `3003f`):
a linha do SDL GameControllerDB continua certa.

**Onde a correção é aplicada** (`ds4_fix_all`, em `src/bt/bfbt-agent.c`): no registro SDP
guardado em `/var/lib/bluetooth/<adaptador>/cache/<controle>`, que o `bluetoothd` só lê ao
iniciar.

- **No boot e ao ligar o Bluetooth:** `S35bluetooth start` roda `bfbt-agent --fix-hid`
  depois de restaurar os pareamentos e antes do `S40bluetoothd`.
- **Depois de parear:** o agente procura por 15 s o registro novo; quando corrige, reinicia o
  `bluetoothd`, conecta o controle de novo e salva os pareamentos no cartão.

O BlueZ pode regravar o cache ao refazer o SDP numa conexão; o próximo boot corrige de novo.

**Verificado no aparelho:**
1. um `uhid` de teste com o descritor novo e relatórios 0x11 reais gerou Xis (`0x131`),
   `ABS_X`, R2 (`0x137` + `ABS_RY`) e PS (`0x13c`);
2. com o cache corrigido e o `bluetoothd` reiniciado, o kernel passou a usar o descritor
   novo (`cmp` com o sysfs) e o DualShock 4 real gerou 147 eventos em 10 s (L1, R2,
   direcional, analógicos);
3. o testador do SC Controller passou de 500 eventos.
