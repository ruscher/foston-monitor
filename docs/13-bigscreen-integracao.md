# 13 · Integração com o Big Screen Monitor Display

## Hoje (Windows CE)

O cliente `bsmd-ce` (repositório big-screen-monitor-display, `device-clients/wince`) roda
no Windows CE original e fala **FSP v1** com o serviço do PC pela porta USB serial
(`045e:00ce`, driver `ipaq`). Os 7 temas, a barra de controle e o toque funcionam
(ver README).

### Correção feita nesta etapa: bsmd-ce 1.2.1

O driver USB do FOSTON grava a troca de cliente como padrão de boot. Depois de usar o
painel, o aparelho passava a ligar como porta serial e não aparecia mais como pendrive
para instalar o cliente. O bsmd-ce 1.2.1 devolve `DefaultClientDriver` ao valor
anterior logo após trocar para serial.

- Arquivos: `device-clients/wince/src/main.c` e o `.exe` empacotado (+ `.sha256`).
- Testes do FOSTON no repositório: 49 passam.
- **No aparelho: NÃO TESTADO** (o mesmo mecanismo foi validado no `bfagent`).

## No BigFoston OS (Linux) — funcionando

O app **Big Screen** do shell (`src/ui/core/bigscreen.c`, `screens/scr_bigscreen.c`)
fala o mesmo **FSP v1** do bsmd-ce, mas por **TCP na rede USB**: `192.168.7.2:7707`.
Assim a rede, o SSH e o console serial (`ttyGS0`) continuam como estão.

- O núcleo do protocolo é o mesmo (`src/ui/third_party/fsp`, cópia sem alterações de
  `device-clients/wince/src/fsp.c/.h`).
- Espera com instruções e status; conectado, o quadro do PC cobre a tela inteira e os
  toques voltam já calibrados; segurar o canto superior esquerdo 3 s sai.
- `bigfoston-shell --open bigscreen` abre direto no app.

No PC (big-screen-monitor-display, branch `feat/foston-fs707dc`):

- `TcpTransport` (mesma interface do serial) e o driver `FostonLinuxDisplay`
  (perfil `foston_bigfoston`, janela de 128 KiB);
- reconhecido sozinho pelo número de série do gadget USB (`0525:a4aa`,
  `BIGFOSTON-FS707DC`, posto pelo kernel com `g_cdc.iSerialNumber`) e confirmado pelo HELLO.

Medido com o aparelho (20/11/2026): quadro cheio em 155 ms, animação a **36 quadros/s**,
ida e volta de 28 ms, PING e brilho ok; o framebuffer do FOSTON ficou idêntico ao quadro
enviado. O `src/bigscreen/` (cliente serial separado) fica como referência e teste no PC.
