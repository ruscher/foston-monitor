# Etapa 4 — Internet pelo celular (Bluetooth PAN)

Com "Tethering Bluetooth" ligado no celular, o FOSTON agora usa a internet dele.

## Como funciona (`src/bt/bfbt-agent.c`)

1. **Detecção:** o agente lê os UUIDs de cada aparelho (Device1). Quem anuncia NAP
   (`00001116-…`) pode dar internet.
2. **Conexão:** com o celular pareado e conectado, e fora da lista de desligados, o agente chama
   `org.bluez.Network1.Connect("nap")` (chamada assíncrona). Se falhar, tenta de novo a cada
   30 s.
3. **Interface:** o BlueZ cria a `bnep0` e o agente inicia `udhcpc -i bnep0 -f -R -t 8 -T 3 -A 10`.
4. **Rota e DNS:** o script do udhcpc põe a rota padrão com métrica 0 (ganha da `usb0`, de
   métrica 200) e o DNS marcado `# bnep0` em `/run/resolv.conf`.
5. **Saída:** ao desconectar ou desligar, o agente para o DHCP e desconecta o PAN. A rota e o DNS
   saem junto.

A escolha de cada aparelho fica em `/data/bigfoston/bt-internet-desligada` (lista de endereços).
O padrão é ligado.

## Na tela (Ajustes > Bluetooth)

Na linha do celular aparecem:
- o estado:
  - "Internet pelo celular: conectada (10.124.78.88)";
  - "Internet pelo celular: ligue o Tethering Bluetooth no celular";
  - "Internet pelo celular: pegando endereço...";
  - "Internet pelo celular: desligada";
- um botão de **globo** que liga e desliga o uso da internet daquele aparelho (comando
  `internet ENDEREÇO 1|0` no FIFO do agente).

O estado vem do agente em `/run/bigfoston-bt`:
- linhas `pan=ENDEREÇO|quer|conectado|ip`;
- o shell lê em `btstat` os campos `nap`, `inet_want`, `inet_on` e `inet_ip`.

## Conferido no aparelho

POCO X4 Pro 5G com tethering Bluetooth:
- `bnep0` com 10.124.78.88;
- rota padrão pelo celular;
- DNS acrescentado;
- `ping 1.1.1.1` ok e HTTP ok.

A conexão continuou de pé durante os testes de TV e do plano de energia.
