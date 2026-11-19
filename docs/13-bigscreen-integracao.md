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

## No BigFoston OS (Linux)

O modo **Big Screen Display** do BigFoston OS usa o mesmo protocolo, para que o serviço
do PC e os temas funcionem sem um sistema paralelo.

`src/bigscreen/fsp-linux.c` → `bigfoston-bigscreen`:

- lê FSP de um dispositivo serial (no aparelho: a porta do USB gadget, `/dev/ttyGS0`);
- usa o decodificador `fsp.c` **sem alterações** (copiado do bsmd; manter sincronizado);
- desenha num buffer sombra e só copia para `/dev/fb0` no `PRESENT` (sem quadros rasgados);
- responde `HELLO_ACK` com o mesmo layout do bsmd-ce (`platform = "BigFoston OS"`,
  versão do kernel no lugar da versão do CE), `PRESENTED`, `PONG`, `NAK`, `CREDIT`;
- envia `TOUCH` (down/move/up) a partir de um dispositivo evdev.

Teste ponta a ponta (PC, pseudo-terminal, codificador real do Big Screen):
`python3 -m unittest discover -s tests/bigscreen` — os pixels de retângulos raw, RLE,
deflate e fill chegam **exatos** ao framebuffer somente após `PRESENT`. **Passa.**

### O que falta para funcionar no aparelho

1. Driver do USB device do MT3351 → gadget `g_serial`/ACM (bloqueado: IP USB ainda não identificado).
2. Detecção no serviço do PC: hoje ele procura `045e:00ce` (`ipaq`). Um gadget Linux
   aparece como `ttyACM` com IDs próprios; o driver `foston` do Big Screen precisará
   aceitar esse identificador.
3. Touchscreen no Linux (para o toque voltar ao PC).
