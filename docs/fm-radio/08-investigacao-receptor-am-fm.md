# Investigação: o FOSTON recebe rádio AM ou FM?

**Conclusão: não.** Nenhum componente do FOSTON FS-707DC recebe radiodifusão AM ou FM; por isso
o app não tem as abas "Rádio FM"/"Rádio AM", e nada de recepção é simulado.

| Peça | O que é | Recebe AM/FM? | Evidência |
|---|---|---|---|
| KT Micro KT0801A | transmissor FM | não (só transmite) | `FMC.dll`: só funções de ligar, desligar e frequência de transmissão; a família KT0801/KT0803 é de transmissores |
| Siano (chip 0x2130, "Nova B0") | sintonizador ISDB-T | não no FOSTON | o único firmware no aparelho é `isdbt_nova_12mhz_b0.inp` (ISDB-T 1seg); alguns chips Siano têm um modo de rádio FM com outro firmware, que o aparelho não tem (e não há como obtê-lo do próprio FOSTON) |
| MediaTek MT6611 | Bluetooth | não há indício | o driver `mtk_bthuart.dll` e o patch só falam HCI; nenhuma função ou texto de FM |
| MT3351 "FM-in" (`0x80061204`) | entrada analógica de linha do codec | não é sintonizador | usada pelo áudio da entrada AV; não há chip de rádio ligado a ela |
| Windows CE | 185 módulos XIP + arquivos da ROM | — | nenhum módulo com fm/radio/tuner/rds/kt0 no nome; entre os arquivos da ROM, o único de rádio é o `FMC.dll` (transmissor) |
| Apps do CE (`part2/app`) | Avin, Music, Photo, Setting, ISDBT… | — | nenhum app de rádio; o `FM.xml` (tela "FM") só tem Aberto/Fechado e a frequência de transmissão |
| Registro (`HKLM\Drivers\BuiltIn`) | drivers ativos | — | só `fmc (FMC1)` para rádio |

O FOSTON também não detecta canais livres para transmitir (não há receptor): o app pede que o
usuário escolha uma frequência sem emissora no rádio externo.

## Alternativas

- **Rádio pela internet**: possível no futuro como "Rádio Online" (nunca como AM/FM por antena),
  mas o FOSTON só tem rede pelo USB do PC (ou celular pelo Bluetooth) e o player atual não toca
  fluxos HTTP; não foi incluído agora.
- **Receptor externo**: o USB do FOSTON é de **dispositivo** (Mentor MUSB em modo gadget); não
  há USB host para um sintonizador USB. Um receptor I²C (ex.: módulos de rádio comuns) exigiria
  solda no barramento I²C1 (`0x8002D000`, onde está o KT0801A) e alimentação; não recomendado.
