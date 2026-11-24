# Etapa 5 — Pareamento do celular (POCO X4 Pro 5G)

Queixa: várias tentativas para parear; depois conecta, mas pede o código de pareamento a cada
conexão.

## O que os registros mostraram

1. **O modo performance derrubava o celular.**
   - O `bluetoothd` foi parado e religado várias vezes com o celular conectado (08:01, 08:11,
     08:33, 08:43), cada vez que o modo entrava ou saía por causa do controle.
   - A cada religada o enlace caía. O celular tentava de volta, às vezes antes de o BlueZ ter as
     chaves carregadas, e o pareamento era refeito.
   - **Corrigido:** qualquer aparelho Bluetooth conectado segura o Bluetooth
     ([02](02-modo-performance.md)).
2. **As chaves criadas pelo celular demoravam a ir para o cartão.**
   - O rootfs está na RAM. O agente copiava `/var/lib/bluetooth` para o cartão só depois de um
     pareamento iniciado pelo FOSTON.
   - Um pareamento iniciado pelo celular ficava só na RAM. No boot seguinte a chave sumia, o
     celular ainda a tinha e o BlueZ pedia o código de novo; no log, "Permission denied" do avdtp
     (chave não confere).
   - **Corrigido:** o agente olha a data mais nova dos arquivos `info` e grava no cartão 2 s
     depois de qualquer mudança, venha de onde vier.

## Ainda em análise

Uma captura HCI longa (`bfsnoop`, 40 min) fica rodando no aparelho para registrar uma conexão e
um pareamento **iniciados pelo celular**. Ela vai mostrar o pedido de chave do controlador, a
resposta do BlueZ e a falha de autenticação, se houver.

Para analisar, no PC:

```
btmon -T -r captura.snoop
```

Procure por `Link Key Request`, `Link Key Request Negative Reply`, `PIN Code Request` e
`Authentication Failure`. O conteúdo das chaves não deve ser impresso nem copiado para fora do
aparelho.

A conexão iniciada pelo celular aparecia como "não testada" nos docs do Bluetooth
([../bluetooth-integration/18](../bluetooth-integration/18-reconnection.md)) e continua como item
aberto até a captura confirmar.
