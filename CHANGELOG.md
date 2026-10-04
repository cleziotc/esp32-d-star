# Changelog

## v0.1.9
- Corrige a consulta online ao GitHub usando um manifesto direto em `raw.githubusercontent.com`, evitando a cadeia de redirecionamentos usada para localizar `latest.json` nas Releases.
- Aguarda a sincronização do relógio via NTP antes de iniciar TLS/HTTPS e informa claramente quando o relógio ainda não está válido.
- Usa o bundle completo de certificados raiz do ESP-IDF e registra também o `errno` em falhas HTTPS para facilitar diagnóstico.
- A página **Atualização de Firmware** passa a exibir os detalhes da release instalada e da última release publicada.
- Mostra versão, build, ESP-IDF, alvo, partição ativa, hashes, tamanho, data de publicação e notas da release.
- A consulta ao GitHub é disparada automaticamente na primeira abertura da página de atualização.
- O workflow de release publica automaticamente um manifesto leve em `update/latest.json` para as próximas atualizações OTA.
- Mantém a atualização manual por arquivo `.bin` como caminho de recuperação.

## v0.1.8
- Adiciona verificação de atualização online diretamente no GitHub.
- Adiciona instalação OTA online sem baixar o arquivo `.bin` no computador.
- Usa HTTPS com o bundle de certificados do ESP-IDF.
- Download e gravação ocorrem em tarefa separada para não bloquear o servidor web.
- Exibe progresso da atualização no dashboard.
- Valida tamanho recebido e a imagem de aplicação antes de trocar a partição de boot.
- Mantém a atualização manual por `.bin` como alternativa de recuperação.
- GitHub Actions continua gerando automaticamente firmware, checksums e `latest.json`.

## v0.1.7
- Corrige reinicialização/instabilidade ao abrir a página **Redes** com listas grandes de reflectores.
- `/api/reflectors` passa a enviar JSON em streaming, sem montar milhares de objetos cJSON na RAM.
- Tabela interna de hosts compactada e ampliada de 2200 para 4600 entradas.
- Corrige truncamento que impedia carregar completamente REF e XRF.
- Sincronização valida XLX / REF / XRF / DCS antes de substituir a lista ativa.
- Retentativas automáticas de hosts limitadas a uma a cada 5 minutos após falha.
- Boot log registra o motivo do último reset.

## v0.1.6
- Até 5 perfis Wi-Fi salvos.
- Pesquisa de redes Wi-Fi.
- Reflectores carregados do `DStar_Hosts.json`.
- Sincronização automática dos hosts às 03:00 (UTC-3).
- Botão de atualização manual dos arquivos de hosts.
