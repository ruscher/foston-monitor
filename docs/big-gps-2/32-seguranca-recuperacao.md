# Segurança e recuperação

- Nada desta frente mexe no Windows CE, no EBoot ou na NAND. A atualização do Linux usa o
  caminho que já existia: `BigFoston/update` no cartão, com o `SHA256SUMS` escrito por último.
  O agente aplica na partida e guarda a versão anterior (`*.old`). Uma atualização recusada
  deixa o Linux anterior rodando (`update.bad`).
- Os pacotes novos do cartão (`alertas/*.bga`, `rotas/*.bgl`, `voz/*.bgv`) são **opcionais e
  separados**. Sem eles o Big GPS funciona como antes, e um arquivo corrompido é recusado ao
  abrir: a validação de tamanho e cabeçalho é coberta pelos testes. Os mapas, rotas e índices
  existentes não mudaram de formato.
- Voltar para a versão anterior: copiar `BigFoston/recuperacao/linux-0.2.0` (ou a 0.5.1
  publicada) para `BigFoston/update` e desligar/ligar.
- Preferências novas no `settings.conf`: `map_alerts`, `map_persp`, `map_veh_N`. Leituras
  inválidas caem no padrão.
- **Lição registrada:** o `reboot` do Linux desliga o FOSTON (docs/17-recuperacao.md). Para
  aplicar uma atualização, peça para desligar e ligar no botão.
