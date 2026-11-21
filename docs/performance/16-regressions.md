# Regressões encontradas no caminho

| Achado | Causa | Situação |
|---|---|---|
| toque injetado não funcionava com o shell novo | a ferramenta de teste repetia as mesmas coordenadas e o kernel descarta eventos ABS iguais ao anterior; não era o shell | ferramenta corrigida, toque conferido |
| build quebrou ao regenerar `lv_conf.h` | o gerador estava defasado (GIF ligado à mão no arquivo gerado) | gerador corrigido (`LV_USE_GIF`) |
| leitura de `0x8003523c` travou o barramento e reiniciou o aparelho (sessão do botão de energia) | palavra fora da janela que o WinCE mapeia | registrado; só ler endereços que um driver do CE lê |
