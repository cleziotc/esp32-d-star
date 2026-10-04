# Polar D-Star ESP32

Firmware dedicado em **ESP-IDF 6.1** para ESP32-S3, com interface web, Wi-Fi, OTA e preparação para comunicação com placas MMDVM.

## Versão estável atual

**v0.1.7** — validada em hardware ESP32-S3.

Principais recursos atuais:

- dashboard web responsivo;
- AP de manutenção permanente em `192.168.4.1`;
- até 5 redes Wi-Fi salvas;
- atualização OTA manual por arquivo `.bin`;
- sincronização de XLX / REF / XRF / DCS pelo `DStar_Hosts.json`;
- atualização automática dos hosts às 03:00 (UTC-3);
- persistência das configurações em NVS;
- base preparada para a integração MMDVM da v0.2.

## Build local

### Windows

```powershell
python tools\generate_web_assets.py
python "$env:IDF_PATH\tools\idf.py" set-target esp32s3
python "$env:IDF_PATH\tools\idf.py" build
```

### Linux

```bash
./build-linux.sh
```

O firmware OTA é gerado em:

```text
build/polar_dstar_esp32.bin
```

## Build automático

Cada alteração na branch `main` é compilada automaticamente pelo GitHub Actions usando **ESP-IDF 6.1**.

Tags no formato `vX.Y.Z` disparam o workflow de release, que publica os binários, `SHA256SUMS.txt` e `latest.json`.

## Base pública de indicativos

A base mestre está em:

```text
csv/database.csv
```

Ela será usada como fonte para a consulta de indicativo, nome e localidade pelo Polar D-Star. A indexação otimizada para o ESP32 será gerada separadamente para evitar o download integral da base.

## Próximas etapas

- v0.2: UART ESP32-S3 ↔ MMDVM e `GET_VERSION`;
- atualização OTA online a partir das releases do GitHub;
- índice leve da base `csv/database.csv` para consulta por indicativo.
