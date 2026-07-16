# esp-stock-data

Dashboard de mercado financeiro (cripto + ações) numa tela touch, rodando em
firmware ESP-IDF sobre um ESP32-P4 com display MIPI-DSI de 7".

![target](https://img.shields.io/badge/target-esp32p4-blue)

## Hardware

- **Placa:** Guition JC1060P470 v1.1 (ESP32-P4NRW32 + ESP32-C6-MINI-1U-N4,
  32MB PSRAM, 16MB flash)
- **Display:** 7", 1024x600, IPS, painel JD9165 sobre MIPI-DSI (2 lanes)
- **Touch:** capacitivo GT911 sobre I2C
- **WiFi/BT:** via ESP32-C6 embarcado (o P4 não tem rádio próprio) ligado por
  SDIO, usando [esp-hosted](https://github.com/espressif/esp-hosted-mcu)

A pinagem usada (`main/board_config.h`) vem da firmware de referência do
fabricante e **não foi validada contra uma unidade física**. Se o display ou
o touch não subirem, comece por aí -- veja o aviso em [CLAUDE.md](CLAUDE.md).

## O que faz hoje

- Conecta no WiFi (via ESP32-C6) e mostra o status da conexão (SSID, IP, ícone)
  numa barra no topo da tela.
- Busca preço de criptomoedas (CoinGecko) e ações (Alpha Vantage) em HTTPS e
  mostra um card por símbolo com preço, variação % e "atualizado há Ns".
- Lista de símbolos monitorados e credenciais configuráveis via
  `idf.py menuconfig`, sem hardcode no código.

Veja [CLOUD.md](CLOUD.md) para os detalhes de como a conectividade e as APIs
de mercado estão montadas, e o roadmap do que vem a seguir (tela de
configuração on-device para adicionar símbolos sem recompilar, etc).

## Build e flash

ESP-IDF `v5.4.4`, alvo `esp32p4`.

```powershell
. C:\esp\v5.4.4\esp-idf\export.ps1     # ajuste o caminho se o seu setup for diferente
idf.py set-target esp32p4              # só na primeira vez / após apagar build/
idf.py menuconfig                      # configurar WiFi + API keys + símbolos, em
                                        # "Stock Ticker Configuration"
idf.py -p COMx build flash monitor
```

### Configuração (`idf.py menuconfig` → Stock Ticker Configuration)

| Menu | Opção | Descrição |
|---|---|---|
| WiFi | SSID / Senha | rede a conectar |
| Market Data API | Alpha Vantage API key | grátis em alphavantage.co/support/#api-key -- necessária só para ações |
| Market Data API | CoinGecko coin IDs | ex: `bitcoin,ethereum` (sem key) |
| Market Data API | Stock symbols | ex: `AAPL,MSFT` (precisa da key acima) |
| Market Data API | Refresh interval | Alpha Vantage free tier é limitado a 25 req/dia -- veja CLOUD.md |

## Estrutura

```
main/
  app_main.c        orquestra o boot: display -> market task -> UI -> wifi
  board_config.h     pinagem/timings do painel e do touch
  bsp_display.c/h    bring-up MIPI-DSI + GT911 + LVGL (esp_lvgl_port)
  wifi_manager.c/h   WiFi station com reconexão automática + status p/ UI
  market_api.c/h     task de fundo: HTTPS + JSON -> lista de market_item_t
  ui.c/h             tela LVGL: status WiFi + cards de preço
components/
  esp_lcd_jd9165/    driver do painel MIPI-DSI (vendorizado, não está no
                      component registry)
```

Contexto de arquitetura mais detalhado em [CLAUDE.md](CLAUDE.md).
