# Conexão e reconexão

- O `bfbt-agent` conecta os aparelhos pareados e confiáveis 3, 15 e 60 s depois de o adaptador
  ligar; um comando do usuário (conectar, desconectar, esquecer) cancela as tentativas restantes.
  As tentativas automáticas não sobrescrevem o resultado do último comando do usuário.
- **Falha corrigida (22/11/2026):** com o POCO X4, a conexão falhava (AVDTP Discover sem resposta,
  depois "Connection refused" do celular) por causa da troca de papel no meio da abertura do áudio
  ([03](03-bluez-stack.md)). Sem a troca: 5/5 conexões, reconexão no boot sem erro.
- Depois de conectar, o celular passa a mestre do enlace (melhor contra interferência):
  [03](03-bluez-stack.md).
- Ao desligar/reiniciar o FOSTON, o BlueZ desconecta os aparelhos antes (`Powered=false`).

**Ainda não testado depois da correção:** a conexão iniciada pelo celular.
