# Relatório final — desempenho do BigFoston OS 0.2.0

Aparelho FOSTON FS-707DC (ARM1176 de 468 MHz, 128 MB, 800×480 RGB565), 21/11/2026.
Todas as medidas foram feitas no aparelho. A última rodada usou o **firmware instalado**:
kernel 6.12.117 com zram e PSI, atualização aplicada às 21:47 (zImage `976e9c2a`).

## O que melhorou

| | Antes | Firmware instalado |
|---|---|---|
| Arrasto do mapa | 3,6 quadros/s, laço de 261 ms | **17,4 quadros/s**, laço de 41 ms (máx. 127) |
| Rotação do mapa | 5,1 quadros/s | **12,8 quadros/s** |
| Navegação: pior volta do laço | 722 ms | **316 ms** (p95 30 ms) |
| Zoom: pior volta do laço | 172 ms | **120 ms** |
| GPS bloqueado no cartão | ~20 % de iowait | **0 %** |
| Shell parado | 57 acordadas/s | **4 acordadas/s** |
| Memória livre mínima (roteiro inteiro) | 87,0 MB | 77,5 MB; nenhum swap e nenhuma falta maior |

Na rodada com o firmware instalado, a navegação teve um quadro do motor de 1,3 s, quando
uma rota nova foi carregada do cartão. Ele roda em outra thread e não trava a interface;
a pior volta do laço nessa fase foi de 316 ms. A tabela detalhada, com as etapas, está em
[14](14-benchmarks-before-after.md).

## O que mudou

1. **GPS:** os arquivos do `\ResidentFlash` agora ficam em RAM e vão para o cartão a cada
   2 min, só se mudaram. Assim o `close()` do FAT com a opção `flush` deixou de dormir
   ~100 ms duas vezes por segundo ([11](11-sd-io.md)).
2. **LVGL:** criei o patch 0005, que deixa de desenhar as telas cobertas por uma tela
   opaca. A tela agora é desenhada numa passada, com cache de sombras ([08](08-lvgl-optimization.md)).
3. **Mapa:** os rótulos são compostos no próprio quadro, o fundo só é pintado nas faixas
   descobertas, os controles são opacos e o motor roda com nice 10. Há níveis de detalhe
   Econômico, Equilibrado e Detalhado, mais o Automático ([09](09-gps-map-engine.md)).
4. **Laço principal:** o toque e o botão de energia são atendidos por evento (`poll`)
   ([12](12-scheduler-tasks.md)).
5. **Serviços:** o BusyBox passou a rodar comandos simples sem criar processo, e o vigia
   do cartão usa só comandos internos do shell.
6. **Ajustes > Desempenho:** escolhe o detalhe do mapa. Todas as opções funcionam.
7. **Ferramentas:** `bf-perf-monitor`, o roteiro `BIGFOSTON_BENCH`, o perfilador
   `BIGFOSTON_PROF` e `prof-resolve.py` ([03](03-cpu-profiling.md)).

## O que foi estudado e não adotado (com medida)

- **zram:** não é ativado. Sobra memória, e descomprimir custa ~0,3 ms por página
  ([05](05-zram.md)).
- **Swap no cartão:** não foi adotado. O cartão é FAT32, não seria seguro e não traria
  ganho ([06](06-swap-sd.md)).
- **jemalloc e outros alocadores:** não foram adotados. O `malloc` fica abaixo de 1 % no
  perfil ([07](07-allocators.md)).
- **SCHED_FIFO:** não foi usado. Um nice mais baixo para o motor resolveu sem risco de
  travar o sistema ([12](12-scheduler-tasks.md)).

## Sistema parado (firmware instalado)

CPU total de **~11 %**, medida com o `bf-perf-monitor` a cada 2 s e sem listar threads.
Ela se divide assim:

- GPS: ~4-6 %. É a leitura serial que imita o WinCE mais o motor MTK. Não mexi nela para
  não arriscar o GPS.
- Vigia do cartão: 2-4 %. Ele roda um `sleep 1` por segundo.
- Shell: ~2 %.
- O resto é kernel e rede USB.

Não há leitura nem escrita no cartão com o aparelho parado.

## Limitações e o que falta

- A banda de memória (58 MB/s) limita o arrasto a ~17-19 quadros/s ([19](19-known-limitations.md)).
- O vigia do cartão poderia usar um evento do kernel (uevent) em vez de checar a cada
  segundo. Isso ficou para depois.
- **Não testado:** navegação real em movimento, uso por horas, música com navegação e voz
  ao mesmo tempo e troca do cartão com o mapa aberto ([15](15-stress-tests.md)). O toque
  por evento foi conferido só com toques injetados no evdev.

## Como voltar atrás

Está descrito em [17](17-recovery.md). Gere o firmware do commit anterior e aplique pelo
mecanismo de atualização (`linux-update.py stage`, depois desligar e ligar). Cada mudança
também pode ser desligada sozinha: `BFGPS_NO_MIRROR=1` desliga o espelho do GPS, e
Ajustes > Desempenho > Detalhado volta ao desenho antigo do mapa. Nenhuma partição, nem
o bootloader, nem o WinCE, nem a NAND foram alterados.
