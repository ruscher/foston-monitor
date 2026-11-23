# Atualização e recuperação

- `tools/bigfoston/linux-update.py stage` grava a versão nova em `BigFoston/update` no cartão; no
  boot seguinte o agente do CE a instala na `\ResidentFlash`.
- **Agente 0.6.2:** apaga sobras (`.old`, `.new`), apaga o `SHA256SUMS` instalado, instala arquivo
  por arquivo (cópia `.new` conferida e troca; sem espaço, substitui no lugar e confere) e grava o
  `SHA256SUMS` por último. O boot automático exige o `SHA256SUMS`: um conjunto incompleto nunca é
  iniciado, e a atualização interrompida continua no boot seguinte. Aplicada assim em 22/11/2026
  ("APLICADA: zImage 2101952, initrd.img 3709568").
- O agente 0.6.1 nunca apagou o `.old` (caminho com uma barra só): a recusa "erro 112" de
  22/11/2026 09:18 foi o `.old` da 0.2.0 ocupando 3,6 MB.
- **Voltar à 0.2.0:** copiar `BigFoston/recuperacao/linux-0.2.0` para `BigFoston/update` e desligar e
  ligar. Cópia fiel também no PC: `backup/2026-11-22-linux-instalado-0.2.0/` (fora do git).
- O agente fica instalado pelo `bfagent.py install-agent` numa sessão no CE (ligar com o cabo USB;
  o agente cancela o boot automático quando o PC fala com ele).
