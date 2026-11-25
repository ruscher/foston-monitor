# Recuperação

O navegador não toca no boot, na NAND nem no Windows CE.

| Problema | O que fazer |
|---|---|
| Motor travado ou página pesada | ✕ para parar; sair do app encerra o motor. Se o motor cair, a tela oferece "Tentar de novo". |
| Remover o navegador | apagar `BigFoston/web/` no cartão: o app mostra que o motor não está instalado; o resto do sistema não muda |
| Voltar a uma versão anterior do motor | trocar a pasta `BigFoston/web/` pela anterior (o `bfweb` não guarda nada no cartão) |
| Apagar dados de navegação | menu → Histórico → Limpar; favoritos e WebApps em `<dados>/web/*.tsv`; cookies só existem na RAM (somem ao reiniciar) |
| Shell novo com problema | `linux-update.py` guarda a versão anterior como `*.old`; cópia de recuperação em `BigFoston/recuperacao/` (ver `docs/05-bootloader.md`) |
| Internet do PC | desfazer o compartilhamento: `nmcli c modify bigfoston ipv4.method manual` |
