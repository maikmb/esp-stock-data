# esp-stock-data

Firmware ESP-IDF (C) para um dashboard de mercado financeiro em uma tela
touch, rodando em uma placa ESP32-P4 com display MIPI-DSI de 7".

## Hardware

- **Placa:** Guition JC1060P470 v1.1 (ESP32-P4NRW32 + ESP32-C6-MINI-1U-N4)
- **Display:** 7", 1024x600, IPS, painel JD9165 sobre MIPI-DSI (2 lanes)
- **Touch:** capacitivo GT911 sobre I2C
- **WiFi/BT:** via ESP32-C6 embarcado, ligado ao P4 por SDIO (esp-hosted) —
  o P4 não tem rádio próprio
- **PSRAM:** 32MB hex-mode / **Flash:** 16MB
- Pinagem em [main/board_config.h](main/board_config.h). Veio da firmware de
  referência do fabricante, **não foi validada fisicamente por mim** — se o
  display/touch não subirem, comece checando esses pinos contra o esquemático
  da sua revisão de placa. Detalhes completos em [CLOUD.md](CLOUD.md).
- Já apareceu em teste real um crash no boot, *antes* do display inicializar,
  que parecia bug de display mas era o `esp_hosted` (ponte WiFi P4↔C6)
  falhando ao alocar seu pool de buffers em RAM interna. Corrigido via
  `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` em `sdkconfig.defaults` — ver
  CLOUD.md antes de investigar qualquer "display não inicializa" a fundo.

## Arquitetura do firmware (main/)

- `board_config.h` — pinagem e timings do painel
- `bsp_display.c/h` — bring-up do display MIPI-DSI + touch GT911 + LVGL
  (via `esp_lvgl_port`)
- `wifi_manager.c/h` — WiFi station, reconexão automática, expõe status
  para a UI via callback + snapshot thread-safe
- `market_api.c/h` — task em background que consulta CoinGecko (cripto) e
  Alpha Vantage (ações) via HTTPS/cJSON e mantém uma lista de `market_item_t`.
  Lista editável em runtime (`market_add_item`/`market_remove_item`,
  persistida em NVS `market_cfg`; Kconfig é só seed de fábrica) e
  `market_lookup()` classifica ticker digitado como cripto vs ação via
  CoinGecko `/search`
- `ui.c/h` — tela LVGL: barra de status WiFi + cards de preço por símbolo
- `ui_wifi.c/h` — painel WiFi em overlay: scan de redes, conectar com senha
  via teclado na tela, detalhes da conexão + desconectar. Só usa a API do
  `wifi_manager` (nunca `esp_wifi.h` direto)
- `ui_symbols.c/h` — painel de tickers em overlay (engrenagem na barra):
  listar/remover/adicionar símbolos; classificação cripto vs ação via
  `market_lookup()`. Só usa a API do `market_api`
- `ui_theme.h` — paleta de cores compartilhada da UI
- `app_main.c` — orquestra a ordem de start: display → market task → UI →
  wifi

Componente próprio: [components/esp_lcd_jd9165/](components/esp_lcd_jd9165/)
(driver do painel, vendorizado da firmware do fabricante — não existe no
component registry).

## Convenções

- Config de usuário (API keys, símbolos monitorados, intervalo de refresh)
  fica em `main/Kconfig.projbuild`, ajustável via `idf.py menuconfig` →
  "Stock Ticker Configuration". Não hardcode esses valores no `.c`.
- `sdkconfig` e `sdkconfig.old` são gerados e não versionados — mudanças
  persistentes de configuração vão em `sdkconfig.defaults`.
- Dependências gerenciadas (LVGL, esp_lvgl_port, esp_lcd_touch_gt911,
  esp_wifi_remote) ficam em `main/idf_component.yml`, resolvidas para
  `managed_components/` no build (também não versionado).
- Toda chamada LVGL fora da própria task do `esp_lvgl_port` precisa do lock
  (`bsp_display_lock()` / `bsp_display_unlock()`) — ver `ui_update_wifi_status`
  e `ui_refresh_market` como exemplo. O lock é recursivo, então callbacks
  aninhados podem tomá-lo de novo sem deadlock.
- Credenciais WiFi vivem **só em NVS** (namespace `wifi_cfg`), gravadas pelo
  painel WiFi da tela (`ui_wifi.c`). Não existe SSID/senha no Kconfig — num
  device zerado o WiFi é configurado inteiramente on-screen.
- As fontes Montserrat embutidas do LVGL só cobrem ASCII — não use acentos
  em strings exibidas na tela (por isso "BOLSA"/"atras" em vez de
  "AÇÃO"/"atrás").

## Build

ESP-IDF instalado localmente em `C:\esp\v5.4.4\esp-idf`. Alvo: `esp32p4`.

```powershell
. C:\esp\v5.4.4\esp-idf\export.ps1
idf.py set-target esp32p4   # só na primeira vez / se o build/ for apagado
idf.py build
idf.py -p COMx flash monitor
```

## Roadmap (contexto do que vem a seguir)

WiFi (`ui_wifi.c`) e lista de tickers (`ui_symbols.c`) já são configuráveis
pela tela, com persistência em NVS. Próximos passos prováveis: SNTP para
horário real, mapear nomes de cripto para tickers curtos nos cards, e
avaliar provedor de ações com free tier menos restritivo. Ao evoluir,
mantenha `market_api.c` como a única camada que fala com as APIs externas --
`ui.c`/`ui_symbols.c` não devem saber nada sobre CoinGecko/Alpha Vantage
além do que a API do `market_api` expõe.
