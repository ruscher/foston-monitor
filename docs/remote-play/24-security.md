# Segurança

- **Pareamento:** o PIN do Moonlight/Sunshine.
  - O FOSTON sorteia 4 dígitos (`/dev/urandom`) e mostra; o usuário aprova no computador.
  - Nada é aceito em silêncio: sem aprovação o host não responde à lista de apps nem abre
    sessão.
- **Identidade:** certificado do cliente gerado no FOSTON (`mkcert` da libgamestream).
  - A libgamestream confere a assinatura do servidor no pareamento.
  - As sessões usam HTTPS com o certificado pareado.
- **Entrada:** pelo canal de controle cifrado da sessão (chave AES de `gs_start_app`). Não há
  porta nova, nem no FOSTON nem no PC.
- **Linhas do host e do shell:**
  - nomes limpos (sem quebras de linha e sem `|`);
  - números limitados;
  - até 200 apps;
  - linhas de comando de até 512 bytes.

## Limitação conhecida: as chaves do cliente no cartão

As chaves ficam em `/data/bigfoston/remoteplay` (`key.pem`, `client.p12`), que é o cartão SD em
vfat. O vfat não tem permissões por arquivo, então elas ficam legíveis para qualquer processo do
FOSTON.

- **Quem tiver o cartão** pode abrir sessões no PC pareado. O remédio é desparear o FOSTON no Big
  Remote Play ou no Sunshine.
- **Um processo comprometido do FOSTON** (por exemplo o navegador, que roda sem privilégios) pode
  ler as chaves.

Não há armazenamento seguro no aparelho. Uma melhora possível é guardá-las cifradas com uma chave
derivada do número de série do aparelho (protege o cartão tirado do aparelho, não um processo
local). Pendente.
