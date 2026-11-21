# 03 · Engenharia reversa: como o FS-707DC foi estudado

Nenhuma documentação pública do MT3351 foi encontrada (mapa de registradores, BSP,
port Linux). Todo o conhecimento do hardware vem do **próprio aparelho**, por três
fontes, nesta ordem de confiança:

1. **Medição ao vivo** com o agente `bfagent` (tabelas de página, RAM, contadores,
   comparação de framebuffer, leitura de registradores sem efeito colateral).
2. **Código da OAL e dos drivers do Windows CE**, extraídos do backup da NAND e
   desmontados (`tools/bigfoston/oal_xref.py`, `tools/bigfoston/cerom.py`).
3. **Registro do Windows CE** (`MemBase`, `Irq`, nomes de DLL).

Anúncios comerciais (64 MB de RAM, "ARM926", 468 MHz) só são aceitos quando
uma medição os confirma. A CPU, por exemplo, é ARM1176 e não ARM926.

## O agente `bfagent` (Windows CE)

Código: [`src/wince/agent/`](../src/wince/agent/). Executável de 35 KB, compilado com
CeGCC 0.59.1. É instalado no cartão SD e aberto pelo atalho **Navegação**
(Ajustes → Caminho da navegação → `BigFoston\bfagent.exe`).

Ao abrir, ele se copia para `\Temp` (RAM), troca o cliente USB para `Serial_Class` e
atende o protocolo **BFP** (`bfp.h`) pela porta USB serial (`045e:00ce`, `/dev/ttyUSB*`).
No PC: [`tools/bigfoston/bfagent.py`](../tools/bigfoston/bfagent.py).

| Comando | O que faz | Efeito no aparelho |
|---|---|---|
| `info`, `cpu` | sistema, CP15 (lido em modo kernel) | nenhum |
| `mmu` | decodifica as tabelas de página vivas | nenhum |
| `phys` | lê memória física (mapeamento **somente leitura**, `PAGE_READONLY`) | nenhum* |
| `disk-info`, `disk-dump` | geometria e cópia bruta de dispositivos de bloco | nenhum |
| `reg`, `ps`, `ls`, `get` | registro, processos, arquivos | nenhum |
| `shot` | captura de tela (RGB565 → PNG) | nenhum |
| `put`, `exec`, `kill` | enviar arquivos, executar, encerrar | só no sistema de arquivos |
| `boot` | carrega e inicia um kernel (ver [05-bootloader.md](05-bootloader.md)) | Windows CE para até o reset |
| `quit [--restart]` | sai (devolve o USB) ou se reinicia a partir do cartão | nenhum |

\* Leitura de registradores com efeito colateral (FIFO de UART, status de interrupção do
USB) é evitada deliberadamente; o host só lê faixas que o próprio CE mapeia.

**Única escrita no registro:** depois de trocar o cliente USB, o agente devolve
`DefaultClientDriver` ao valor original (o driver do aparelho grava a troca como padrão
de boot; ver [01-hardware.md](01-hardware.md)).

### Link USB serial — limites medidos

| Direção | Medição |
|---|---|
| aparelho → PC | ~1,2 MB/s (dump da NAND de 126 MB em 1 min 44 s) |
| PC → aparelho | pedidos de 16 KB **perdidos**; 8 KB ok. O PC envia em fatias de 1 KB com 3 ms entre elas → 54 KB/s |
| latência | ~70–80 ms por requisição |

O driver `Serial_Class` guarda só alguns KB enquanto ninguém lê; a versão 0.1.0 do
agente dormia 5 ms quando ocioso e perdia o início de rajadas de 6 KB. A 0.2.0 dorme
no máximo 1 ms e descarta um pedido parcial parado há 500 ms.

### Convivência com o Big Screen Monitor

O serviço do Big Screen também abre portas `045e:00ce`, com trava exclusiva. Ele não entende
BFP (magia `BF` em vez de `FS`), desiste e tenta de novo com espera de 1 a 30 s;
o `bfagent.py` aproveita essa janela, pega a porta e a mantém.

## Ferramentas de análise (PC)

| Ferramenta | Uso |
|---|---|
| `tools/bigfoston/cerom.py` | lista módulos de uma região XIP (ROMHDR → TOC → o32) e as constantes de periféricos de cada um |
| `tools/bigfoston/oal_xref.py` | acha as funções da OAL pelas strings de depuração e desmonta (Capstone), anotando endereços físicos |
| `tools/bigfoston/mkboot.py` | grava intervalo do initrd e bootargs no DTB |

Capstone fica num venv temporário (`pip install capstone`), não é dependência do projeto.

## Backups (não versionados — `backup/` está no `.gitignore`)

| Arquivo | Conteúdo | SHA-256 |
|---|---|---|
| `backup/2026-11-18-volume/` | arquivos do cartão SD (dados pessoais do iGO, contatos BT) | `files.sha256` |
| `backup/2026-11-19-nand/DSK9-msflash.img` | NAND inteira vista pelo CE (MBR + 4 partições) | `a1d7ee88…62d83` |
| `backup/2026-11-19-survey/` | levantamento: registro, módulos, MMU, discos, tela | — |

Os dumps contêm software proprietário (Windows CE, aplicativos do fabricante) e dados
pessoais. **Não devem ser publicados.**

## Testes

`python3 -m unittest discover -s tests/agent` — 8 testes: CRC, enquadramento C↔Python
nos dois sentidos (com lixo e CRC corrompido), arquivos, decodificação de MMU e dump
retomável contra um agente simulado num pseudo-terminal. São testes **de host**: provam
o protocolo e a ferramenta, não o hardware.
