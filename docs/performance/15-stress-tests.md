# Testes de carga

Feitos (21/11/2026):
- Roteiro do mapa repetido 12 vezes seguidas na sessão (cada um ~1 min: arrasto, zoom,
  rotação, navegação simulada) sem queda, sem crescimento de RSS entre execuções
  (navegação 12,4-14,1 MB) e MemAvailable estável (84,8-87 MB).
- Leitura contínua do cartão (35 MB) com medida de atraso da interface ([11](11-sd-io.md)).
- Toque injetado pelo evdev (`tapinj`, escreve em `/dev/input/event0`) para conferir o toque
  por evento: toque abriu Ajustes como no shell antigo.

**Não feitos ainda (precisam do aparelho em uso real ou de horas):** navegação real em
movimento com GPS, uso contínuo por horas, música + navegação + voz juntas, inserção e
remoção do cartão com o mapa aberto.
