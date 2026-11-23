# Downloads

Fluxo: o motor recebe uma resposta que é download (ou o usuário escolhe **Baixar link /
imagem** no toque longo) → `DLREQ` para o shell → `core/webdl.c` → um `bfdl` por arquivo.

| Regra | Como |
|---|---|
| Pasta | `BigFoston/Downloads/` no cartão |
| Nome | o do servidor, sem caminho, sem `\ / : * ? " < > \|`, controles, pontos e espaços nas pontas; até 180 bytes sem partir UTF-8 |
| Sem sobrescrever | `foto.jpg` existente → `foto (1).jpg` (também se houver `.part` ou outro download com o nome) |
| Arquivo pela metade | grava em `<nome>.part`; só renomeia depois de `fsync` e do tamanho conferir |
| Pausar / continuar | pausar encerra o `bfdl` (SIGTERM) e mantém o `.part`; continuar pede `Range` com `If-Range` (ETag ou Last-Modified guardados em `.part.info`); se o servidor mandar o arquivo inteiro, recomeça do zero |
| Limites | recusa acima de 4 GB (FAT32) e sem espaço livre (com 1 MB de folga) |
| Cartão | só leitura, ausente, cheio: erro claro, sem arquivo truncado |
| Rede | só http/https, também nos redirecionamentos (até 10); certificado sempre verificado; parado 60 s = falha de rede |
| Fila | até 2 ao mesmo tempo |
| Depois de reiniciar | lista em `<dados>/web/downloads.tsv` (sem cookie); os interrompidos voltam como pausados |
| App fechado | os downloads continuam (o `bfdl` não depende do motor) |
| Abrir | imagem → visualizador; PDF/EPUB/TXT → Livros; áudio → Multimídia; o resto → Arquivos na pasta. Nada é executado. |

Cookie: o motor manda o cookie da sessão para o site do download (para links que exigem
login); o `bfdl` recebe pelo ambiente (`BFDL_COOKIE`, não aparece no `ps`) e ele nunca vai
para o cartão.

Limitação: um download que é a resposta de um formulário POST não pode ser repetido por
URL (o `bfdl` faz GET).

Testado no PC (23/11/2026): inteiro; pausa em 2,87 MB e retomada (SHA-512 igual); caminho
fora da pasta recusado; esquema não HTTP recusado; 404 sem `.part` vazio; certificado
expirado recusado. No FOSTON: falta testar com internet.
