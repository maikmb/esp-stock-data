# esp-stock-data

Dashboard de mercado financeiro (cripto + ações) numa tela touch, rodando em
firmware ESP-IDF (C) sobre um ESP32-P4 com display MIPI-DSI de 7".

![target](https://img.shields.io/badge/target-esp32p4-blue) ![idf](https://img.shields.io/badge/ESP--IDF-5.4.4-green)

## Índice

- [Hardware](#hardware)
- [O que o firmware faz hoje](#o-que-o-firmware-faz-hoje)
- [Arquitetura](#arquitetura)
- [Conectividade e APIs de mercado ("cloud")](#conectividade-e-apis-de-mercado-cloud)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Build e flash](#build-e-flash)
- [Configuração (`idf.py menuconfig`)](#configuração-idfpy-menuconfig)
- [Convenções do projeto](#convenções-do-projeto)
- [Troubleshooting](#troubleshooting)
- [Roadmap](#roadmap)
- [Agentes do Claude Code neste repo](#agentes-do-claude-code-neste-repo)

## Hardware

| | |
|---|---|
| **Placa** | Guition JC1060P470 v1.1 |
| **MCU principal** | ESP32-P4NRW32 (RISC-V dual-core, sem rádio WiFi/BT próprio) |
| **Co-processador de rádio** | ESP32-C6-MINI-1U-N4, ligado ao P4 por SDIO |
| **Display** | 7", 1024x600, IPS, painel **JD9165** sobre MIPI-DSI (2 lanes) |
| **Touch** | capacitivo **GT911** sobre I2C |
| **PSRAM** | 32MB hex-mode |
| **Flash** | 16MB |
| **Outros** | slot microSD, USB UART, USB High-Speed, USB Full-Speed |

> [!WARNING]
> A pinagem em [main/board_config.h](main/board_config.h) vem da firmware de
> referência do fabricante (Guition), **não foi validada contra uma unidade
> física** neste projeto. Se o display ou o touch não subirem, o primeiro
> passo é conferir esses pinos contra o esquemático da sua revisão de placa —
> fabricantes desse tipo de placa costumam mudar pinout entre revisões.
> Mesma ressalva vale para os pinos SDIO usados na ponte com o ESP32-C6 (ver
> [Conectividade](#conectividade-e-apis-de-mercado-cloud)).

## O que o firmware faz hoje

- Conecta no WiFi (via ESP32-C6 embarcado) e mostra o status da conexão
  (ícone colorido, SSID, IP) numa barra fixa no topo da tela.
- Busca preço de criptomoedas (CoinGecko) e ações (Alpha Vantage) por HTTPS
  em background e mostra um card por símbolo: preço, variação % (colorida
  verde/vermelho) e "atualizado há Ns".
- Lista de símbolos monitorados, credenciais e intervalo de atualização são
  100% configuráveis via `idf.py menuconfig` — nada hardcoded no `.c`.
- Reconexão de WiFi automática e permanente (o dispositivo é pensado pra
  ficar ligado numa prateleira mostrando preços, não para desistir depois de
  N tentativas).

Ainda **não** faz (ver [Roadmap](#roadmap)): adicionar/remover símbolos pela
própria tela touch, múltiplas telas, gráficos/sparklines, hora real via NTP.

## Arquitetura

Ordem de boot, orquestrada em [main/app_main.c](main/app_main.c):

```
bsp_display_start()   -- sobe display + touch + LVGL
        │
market_task_start()   -- lê símbolos do Kconfig, inicia task de refresh HTTPS
        │
ui_init()              -- monta a tela (já sabe quantos cards criar)
        │
wifi_manager_start()  -- conecta WiFi; status atualiza a UI por callback
```

| Módulo | Responsabilidade |
|---|---|
| [main/board_config.h](main/board_config.h) | Pinagem e timings do painel/touch (única fonte de verdade sobre hardware) |
| [main/bsp_display.c/h](main/bsp_display.c) | Bring-up do display MIPI-DSI (painel JD9165) + touch GT911 + LVGL via `esp_lvgl_port`. Expõe `bsp_display_lock()`/`unlock()` |
| [main/wifi_manager.c/h](main/wifi_manager.c) | WiFi station, reconexão automática com backoff, expõe status via callback + snapshot thread-safe |
| [main/market_api.c/h](main/market_api.c) | Task de fundo: HTTPS + cJSON contra CoinGecko/Alpha Vantage → lista de `market_item_t` thread-safe |
| [main/ui.c/h](main/ui.c) | Tela LVGL: barra de status WiFi + grade de cards de preço. Só conhece `market_item_t`, nunca fala com as APIs diretamente |
| [main/app_main.c](main/app_main.c) | Orquestra a ordem de inicialização acima |
| [components/esp_lcd_jd9165/](components/esp_lcd_jd9165/) | Driver do painel MIPI-DSI, vendorizado da firmware do fabricante (não existe no component registry do ESP-IDF) |

Regra de camadas: **`ui.c` não sabe nada sobre CoinGecko/Alpha Vantage**, e
`market_api.c` não sabe nada sobre LVGL. Toda troca de provedor de API fica
isolada em `market_api.c`; toda mudança visual fica isolada em `ui.c`.

Qualquer chamada LVGL feita fora da própria task do `esp_lvgl_port` (ex.: dos
callbacks de `wifi_manager` ou `market_api`) precisa do lock —
`ui_update_wifi_status()` e `ui_refresh_market()` são o exemplo de referência.

## Conectividade e APIs de mercado ("cloud")

"Cloud" neste projeto significa **chamadas HTTPS diretas do ESP32 a APIs
públicas** — sem backend próprio, sem broker MQTT, sem plataforma IoT
gerenciada (AWS IoT / Azure IoT). Essa escolha foi conversada explicitamente
com o dono do projeto antes de implementar; alternativas descartadas e o
porquê estão no fim desta seção.

```
ESP32-P4 (app, LVGL, sem rádio)
   │  SDIO (pinos default do esp-hosted: CLK18/CMD19/D0-14/D1-15/D2-16/D3-17/RST54)
   ▼
ESP32-C6 (co-processador WiFi/BT, roda o firmware esp-hosted)
   │  WiFi 802.11
   ▼
Roteador / Internet
   │  HTTPS (TLS 1.2+, cert bundle da mbedTLS)
   ▼
api.coingecko.com  /  www.alphavantage.co
```

- **WiFi sem rádio nativo**: `main/wifi_manager.c` usa a API `esp_wifi.h`
  normalmente — o mesmo código que rodaria num chip com WiFi nativo. Quem faz
  a ponte com o ESP32-C6 é o componente `espressif/esp_wifi_remote`
  (dependência em `main/idf_component.yml`), habilitado via
  `CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y` em `sdkconfig.defaults`. Os pinos
  SDIO usados são os defaults do esp-hosted (os mesmos da
  ESP32-P4-Function-EV-Board oficial da Espressif) — a JC1060P470
  provavelmente segue o mesmo layout de referência, mas **isso não foi
  confirmado contra o esquemático real da placa**.
- **TLS**: `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` +
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` embarcam o conjunto
  padrão de CAs raiz no firmware. Ambos os provedores usam CAs comuns
  (Amazon/DigiCert), cobertas pelo bundle — sem pinning nem certificados
  customizados.

### APIs de dados de mercado

| | Cripto | Ações |
|---|---|---|
| Provedor | [CoinGecko](https://www.coingecko.com/en/api) | [Alpha Vantage](https://www.alphavantage.co/) |
| Auth | nenhuma (endpoint público) | API key grátis via Kconfig |
| Endpoint | `GET /api/v3/simple/price?ids=...&vs_currencies=usd&include_24hr_change=true` | `GET /query?function=GLOBAL_QUOTE&symbol=...&apikey=...` |
| Limite free tier | ~10-30 req/min (sem key) | **25 requisições/dia**, 5/min |
| Batch? | sim, uma request para todos os ids configurados | não, uma request por símbolo |

Por causa do limite baixo da Alpha Vantage, `market_api.c` espaça as
requisições de ações em `STOCK_REQUEST_GAP_MS` (1.3s) e o intervalo de
refresh padrão é 60s. Com poucos símbolos de ação e refresh de 60s, o
orçamento diário de 25 requisições esgota rápido — para acompanhar várias
ações, o certo é trocar de plano ou de provedor.

### Alternativas de "cloud" descartadas

1. **HTTPS direto a APIs públicas** (escolhida) — mais simples, sem
   infraestrutura própria, o ESP32 é o único "backend".
2. Plataforma IoT gerenciada (AWS IoT / Azure IoT) com broker MQTT buscando
   os dados e repassando ao device — mais complexo, faz sentido se no futuro
   houver múltiplos dispositivos ou processamento centralizado.
3. Backend próprio — útil se o roadmap incluir agregação de várias fontes,
   cache, ou lógica que não deveria rodar no firmware.

Se o projeto crescer para múltiplas telas na mesma rede, um backend próprio
(opção 3) que os dispositivos consultem passa a fazer mais sentido do que
cada um bater direto nas APIs externas.

## Estrutura do repositório

```
main/
  app_main.c          orquestra o boot: display -> market task -> UI -> wifi
  board_config.h       pinagem/timings do painel e do touch
  bsp_display.c/h       bring-up MIPI-DSI + GT911 + LVGL (esp_lvgl_port)
  wifi_manager.c/h      WiFi station com reconexão automática + status p/ UI
  market_api.c/h        task de fundo: HTTPS + JSON -> lista de market_item_t
  ui.c/h                 tela LVGL: status WiFi + cards de preço
  Kconfig.projbuild     opções de menuconfig (WiFi, símbolos, API keys)
  idf_component.yml    dependências gerenciadas (lvgl, esp_lvgl_port, ...)
components/
  esp_lcd_jd9165/      driver do painel MIPI-DSI (vendorizado, não está no
                        component registry)
.claude/agents/         subagentes do Claude Code específicos deste projeto
CLAUDE.md               contexto do projeto para sessões futuras do Claude Code
CLOUD.md                versão só da seção de conectividade/APIs, para referência rápida
sdkconfig.defaults      config persistente (PSRAM, wifi_remote, TLS bundle, partição, fontes LVGL)
```

`sdkconfig`, `sdkconfig.old`, `build/` e `managed_components/` são gerados
pelo ESP-IDF e não são versionados (`.gitignore`); `dependencies.lock`
**é** versionado, para builds reprodutíveis.

## Build e flash

ESP-IDF `v5.4.4`, alvo `esp32p4`, instalado localmente em
`C:\esp\v5.4.4\esp-idf`.

```powershell
. C:\esp\v5.4.4\esp-idf\export.ps1     # ajuste o caminho se o seu setup for diferente
idf.py set-target esp32p4              # só na primeira vez / após apagar build/
idf.py menuconfig                      # configurar WiFi + API keys + símbolos
idf.py -p COMx build flash monitor
```

O primeiro build depois de `set-target` (ou depois de mudar
`main/idf_component.yml`) baixa as dependências gerenciadas (LVGL,
`esp_lvgl_port`, `esp_lcd_touch_gt911`, `esp_wifi_remote`) — precisa de
internet e demora mais.

Build validado neste repositório: `idf.py build` completo, **0 erros, 0
warnings**, binário de ~1.44MB cabendo na app partition (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`,
~1.46MB) com ~6% de folga.

## Configuração (`idf.py menuconfig`)

Tudo em **Stock Ticker Configuration**:

| Menu | Opção | Descrição |
|---|---|---|
| WiFi | SSID / Senha | rede a conectar |
| WiFi | Fast retry attempts | tentativas rápidas antes de cair pro backoff de 15s |
| Market Data API | Alpha Vantage API key | grátis em [alphavantage.co/support/#api-key](https://www.alphavantage.co/support/#api-key) — necessária só para ações |
| Market Data API | CoinGecko coin IDs | ex: `bitcoin,ethereum` (sem key necessária) |
| Market Data API | Stock symbols | ex: `AAPL,MSFT,PETR4.SAO` (precisa da key acima) |
| Market Data API | Refresh interval | padrão 60s — ver limite da Alpha Vantage acima |

Nenhuma credencial fica hardcoded em `.c`: tudo entra via Kconfig e cai no
`sdkconfig` (gerado, não versionado). `sdkconfig.defaults` só guarda valores
que fazem sentido compartilhar entre quem clona o repo (config de
PSRAM/TLS/partição/fontes) — nunca SSID, senha ou API key.

## Convenções do projeto

- Config de usuário (SSID, senha, API keys, símbolos, intervalo) fica em
  `main/Kconfig.projbuild`. Não hardcode esses valores no `.c`.
- Mudanças persistentes de configuração de build vão em `sdkconfig.defaults`,
  nunca direto no `sdkconfig` gerado.
- Dependências gerenciadas ficam em `main/idf_component.yml`.
- Este projeto usa **LVGL v9** (`lv_screen_active()`, não `lv_scr_act()`).
  Não copie padrões de exemplos LVGL v8 encontrados por aí sem checar a
  versão pinada em `idf_component.yml`.

## Troubleshooting

- **Display não acende**: confira `main/board_config.h` contra o
  esquemático real da sua placa (ver aviso em [Hardware](#hardware)).
- **WiFi nunca conecta / ESP32-C6 não é detectado**: verifique os pinos SDIO
  do esp-hosted em `idf.py menuconfig` → Component config → ESP-Hosted, e
  compare com o esquemático da JC1060P470 (ver
  [Conectividade](#conectividade-e-apis-de-mercado-cloud)).
- **Cards de ação sempre vazios**: confira se a API key da Alpha Vantage foi
  configurada e se não estourou o limite diário (25 req/dia no free tier) —
  o log do monitor mostra um warning quando isso acontece.
- **Erro de build depois de mexer em `idf_component.yml`/`CMakeLists.txt`**:
  use o subagente `firmware-builder` (ver abaixo) ou rode `idf.py build` e
  confira a lista de `REQUIRES` em `main/CMakeLists.txt` contra os
  `#include` usados.

## Roadmap

- Tela de configuração na própria interface touch para adicionar/remover
  símbolos em tempo real, persistindo em NVS (hoje é só via Kconfig +
  rebuild).
- Mapear ids da CoinGecko para tickers curtos (hoje o card mostra o id
  capitalizado, ex. "Bitcoin", não "BTC").
- Sincronizar hora via SNTP para mostrar horário real da última cotação em
  vez de "Ns atrás".
- Avaliar um provedor de ações com tier gratuito menos restritivo, ou um
  backend próprio com cache, se o número de símbolos crescer.
- Validar a pinagem de `board_config.h` (display, touch, SDIO) contra
  hardware físico.

## Agentes do Claude Code neste repo

- `.claude/agents/firmware-builder.md` — build/flash/monitor e diagnóstico
  de erros de build do ESP-IDF.
- `.claude/agents/market-ui-widget.md` — adicionar novas fontes de dados de
  mercado ou novas telas/widgets LVGL, respeitando a separação
  `market_api.c` ↔ `ui.c`.

Contexto mais amplo do projeto (para uma sessão do Claude Code retomar o
trabalho) fica em [CLAUDE.md](CLAUDE.md).
