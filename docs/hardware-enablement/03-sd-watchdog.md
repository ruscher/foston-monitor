# Vigia do cartão SD por aviso do kernel

Estado: **FUNCIONANDO NO HARDWARE** (script testado ao vivo no aparelho em 22/11/2026;
entra no firmware na próxima atualização).

## Antes

`bigfoston-sd watch` era um laço de 1 s em shell: a cada volta um `sleep 1` (processo novo:
o `sleep` do BusyBox não é NOFORK) e a leitura de `/proc/mounts`. Custo medido: 2–4 % de CPU
com o aparelho parado.

## Como o cartão é detectado

O driver `mt3351-msdc` usa `MMC_CAP_NEEDS_POLL`: o próprio kernel consulta o slot (a
interrupção de detecção, IRQ 15, não tem o reconhecimento conhecido — ver
[sd-controller.md](../sd-controller.md)) e cria ou apaga `/dev/mmcblk0p1`. Cada mudança gera um
uevent `add`/`remove` do bloco. O vigia em shell repetia essa consulta.

## Agora

- `bigfoston-sd watch` roda o `uevent` do BusyBox (socket netlink, nenhuma consulta periódica),
  que chama `bigfoston-sd event` a cada aviso. O evento sai logo (só builtins) se não é a
  partição do cartão; com o aparelho parado chega um aviso a cada 10 s (bateria).
- `add`: espera 1 s (contatos trepidando) e reconcilia; `remove`: reconcilia na hora.
- **Reconciliar:** sem cartão → limpa o que estiver montado (`lost`) e o estado "ejetado";
  cartão presente, não montado e não ejetado → `mount`. Espera uma operação em andamento
  (trava `/run/bigfoston-sd.lock`, até 30 s).
- Conferência a cada 60 s: só para um aviso que chegou enquanto outra operação travava o
  cartão (o `mount` com gravação desliga e religa o controlador e gera `remove`/`add`).
- A proteção de gravação não mudou: gravação só no cartão com a marca do próprio número de
  série; `fsck.fat` num cartão sujo.

## Testes no aparelho

Remoção simulada desligando o controlador (`unbind`/`bind` do `mt3351-msdc`, o mesmo passo que
o próprio script faz para liberar a gravação):

| Caso | Resultado |
|---|---|
| Ejetar, tirar, pôr de novo | aviso de remoção limpou o "ejetado"; montado com gravação em **2,3 s** |
| Tirar sem ejetar (montado só leitura) | desmontado na hora (`cartão removido sem ejetar`) |
| Pôr de novo | montado com gravação e a pasta de dados ligada |
| GPS e shell durante os testes | continuaram rodando |

| Aparelho parado (`bf-perf-monitor -i 2000 -t 0`) | Antes | Depois |
|---|---|---|
| CPU total | 10,9 % | **8,3 %** |
| trocas de contexto/s | ~700 | ~480 |

Ainda não testados com o cartão físico: tirar e pôr de verdade, cartão protegido contra
gravação pela trava, cartão com defeito, reiniciar sem cartão.
