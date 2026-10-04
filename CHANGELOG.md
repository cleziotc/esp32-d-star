# Changelog

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
