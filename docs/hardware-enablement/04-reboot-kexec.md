# `reboot` do terminal com kexec

Estado: **FUNCIONANDO NO HARDWARE** (testado em 22/11/2026: `reboot` pelo SSH voltou por kexec,
sem desligar; entra no firmware na próxima atualização).

O reset do watchdog deixa este aparelho desligado ([04-linux-port.md](../04-linux-port.md)).
O botão Reiniciar da interface já usava `bigfoston-reboot` (kexec); o `reboot` do terminal
chamava o BusyBox direto e desligava.

- `/sbin/reboot` (rootfs-overlay) é um script: `bigfoston-reboot --prepare` confere e carrega
  zImage/initrd/DTB da última atualização (`BigFoston/update.ok`, SHA256SUMS) e marca
  `/kexec/pending`; depois `exec /bin/busybox reboot`. O desligamento é o de sempre (init:
  `rcK`, `umount -a -r`) e o salto acontece no fim (`bigfoston-kexec-exec`).
- Sem kexec possível (sem cartão, atualização pendente, arquivos que não conferem) o script
  diz o motivo e desliga como antes. `reboot -f` continua indo direto ao kernel.
- Sem laço: `bigfoston-reboot` chama `/bin/busybox reboot`, nunca `/sbin/reboot`.
- `poweroff` não mudou.
- `S60gps stop` agora espera o `bfgps` sair (até 5 s): ele grava o espelho do `\ResidentFlash`
  no cartão ao receber SIGTERM, antes de o desligamento desmontar o cartão (medido: sai em
  0,2 s).
