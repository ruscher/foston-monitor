# Recuperação e rollback

- Firmware: as mudanças de kernel, BusyBox e serviços chegam pelo mecanismo de atualização
  (`linux-update.py stage` + desligar/ligar); a versão anterior fica no histórico do git e
  basta gerar e aplicar o commit anterior. O WinCE e o boot original não foram tocados.
- Shell: pode ser trocado ao vivo (`/etc/init.d/S90bigfoston-shell stop`, copiar, `start`);
  o roteiro de teste usa `/tmp/shell-perf` e devolve o shell instalado no fim.
- `bfgps`: `BFGPS_NO_MIRROR=1` volta a usar o cartão direto; os arquivos do espelho são
  gravados no cartão como `.tmp` + `rename`, então um desligamento no meio não deixa um
  arquivo pela metade (fica a versão anterior).
- Detalhe do mapa: Ajustes > Desempenho > Detalhado restaura o desenho antigo.
- LVGL: os patches estão em `src/ui/patches/` (0005 é o único deste trabalho).
