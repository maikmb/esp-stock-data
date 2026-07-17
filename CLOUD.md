# Conectividade e integração com APIs de nuvem

Este documento descreve como o firmware fala com a internet: WiFi, TLS e as
APIs de mercado. "Cloud" aqui significa **chamadas HTTPS diretas do ESP32 a
APIs públicas** -- não há backend próprio, broker MQTT ou plataforma IoT
gerenciada (AWS IoT / Azure IoT) no meio. Essa escolha foi confirmada com o
usuário; ver a seção "Alternativas descartadas" no fim.

## Caminho até a internet

```
ESP32-P4 (aplicação, LVGL, sem rádio)
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

O ESP32-P4 não tem rádio WiFi próprio. `main/wifi_manager.c` chama a API
`esp_wifi.h` normalmente (mesmo código que rodaria num chip com WiFi nativo);
quem faz a ponte para o ESP32-C6 é o componente `espressif/esp_wifi_remote`
(dependência em `main/idf_component.yml`), com
`CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y` em `sdkconfig.defaults`. Os pinos
SDIO usados são os defaults do esp-hosted (os mesmos da ESP32-P4-Function-EV-Board
oficial) -- a JC1060P470 provavelmente segue o mesmo layout de referência,
mas isso **não foi confirmado contra o esquemático real da placa**. Se o C6
não for detectado no boot, esse é o primeiro lugar a checar
(`idf.py menuconfig` → Component config → ESP-Hosted → SDIO pins).

### Bug real já encontrado e corrigido: crash no boot antes do display subir

Em teste real na placa, o firmware travava **antes de `app_main()` rodar** --
o display nunca chegava a inicializar, o que parecia um problema de
display/touch mas não era. O log mostrava:

```
E (1666) HS_MP: mempool create failed: no mem
assert failed: sdio_mempool_create sdio_drv.c:255 (buf_mp_g)
```

O transporte SDIO do `esp_hosted` (a ponte P4↔C6) aloca seu pool de buffers
(~47KB) de RAM interna com capacidade DMA por padrão. No ESP32-P4 essa RAM
interna DMA-capable é escassa e esgota antes desse ponto do boot, mesmo com
os 32MB de PSRAM disponíveis (que o `esp_hosted` não usava por padrão para
esses buffers). A correção -- já aplicada em `sdkconfig.defaults` -- é a
própria opção que o componente expõe pra esse cenário:

```
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y
```

Isso faz os buffers de transporte preferirem PSRAM (o GDMA do P4 alcança
PSRAM através do cache), liberando a RAM interna escassa. Se você clonar
este repo, atualizar `esp_hosted` para uma versão nova, ou apagar o
`sdkconfig`, e o boot voltar a travar com esse mesmo assert, comece
verificando se essa flag ainda está setada.

De passagem, também corrigimos `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` -- estava
no default de 2MB, causando um warning de tamanho de flash divergente em
todo boot (a placa tem 16MB reais).

## TLS

`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` + `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`
embarcam o conjunto padrão de CAs raiz da mbedTLS no firmware. Tanto
`api.coingecko.com` quanto `www.alphavantage.co` usam CAs comuns (Amazon /
DigiCert), cobertas pelo bundle. `esp_http_client_config_t.crt_bundle_attach`
aponta pra esse bundle em `market_api.c` -- não há pinning de certificado
nem certificados customizados.

## APIs de dados de mercado

| | Cripto | Ações |
|---|---|---|
| Provedor | [CoinGecko](https://www.coingecko.com/en/api) | [Alpha Vantage](https://www.alphavantage.co/) |
| Auth | nenhuma (endpoint público) | API key grátis via Kconfig |
| Endpoint | `GET /api/v3/simple/price?ids=...&vs_currencies=usd&include_24hr_change=true` | `GET /query?function=GLOBAL_QUOTE&symbol=...&apikey=...` |
| Limite free tier | ~10-30 req/min (sem key) | **25 requisições/dia**, 5/min |
| Batch? | sim, uma request para todos os ids | não, uma request por símbolo |

Por causa do limite baixo do Alpha Vantage, `market_api.c` espaça as
requisições de ações em `STOCK_REQUEST_GAP_MS` (1.3s) e o intervalo de
refresh padrão é de 60s (`CONFIG_MARKET_REFRESH_INTERVAL_SEC`). Com poucos
símbolos de ação e refresh de 60s, o orçamento diário de 25 requisições
esgota rápido -- para acompanhar várias ações o certo é trocar de plano ou
de provedor (ver Roadmap).

Toda a lógica de rede/parsing fica isolada em `main/market_api.c`; a UI
(`main/ui.c`) só lê `market_item_t` via `market_get_items()`. Trocar de
provedor de API no futuro significa mexer só nesse arquivo.

## Configuração

`idf.py menuconfig` → **Stock Ticker Configuration**:

- **WiFi**: SSID, senha, tentativas de reconexão rápida antes do backoff
- **Market Data API**: chave da Alpha Vantage, lista de ids CoinGecko
  (ex: `bitcoin,ethereum`), lista de símbolos de ações
  (ex: `AAPL,MSFT,PETR4.SAO`), intervalo de refresh

Nenhuma credencial fica hardcoded em `.c` -- tudo entra via Kconfig e cai no
`sdkconfig` (não versionado) ou em `sdkconfig.defaults` se for um valor que
faz sentido compartilhar (o que não é o caso de senha/API key).

## Alternativas descartadas

Perguntei ao usuário o que "trabalhar com cloud" significava para este
projeto antes de implementar. As opções eram:

1. **HTTPS direto a APIs públicas** (escolhida) -- mais simples, sem
   infraestrutura própria, o ESP32 é o único "backend".
2. Plataforma IoT gerenciada (AWS IoT / Azure IoT) com um broker MQTT
   buscando os dados e repassando ao device -- mais complexo, faz sentido
   se no futuro houver múltiplos devices ou processamento centralizado.
3. Backend próprio -- útil se o roadmap incluir agregação de várias fontes,
   cache, ou lógica que não deveria rodar no firmware.

Se o projeto crescer para múltiplas telas na mesma rede, um backend próprio
(opção 3) que os dispositivos consultem passa a fazer mais sentido do que
cada um bater direto nas APIs externas -- vale reconsiderar nesse ponto.

## Roadmap

- Tela de configuração na própria interface touch para adicionar/remover
  símbolos em tempo real, persistindo em NVS (hoje é só via Kconfig/rebuild).
- Mapear ids do CoinGecko para tickers curtos (hoje o card mostra o id da
  CoinGecko capitalizado, ex. "Bitcoin", não "BTC").
- Sincronizar hora via SNTP para mostrar horário real da última cotação em
  vez de "Ns atrás".
- Avaliar um provedor de ações com tier gratuito menos restritivo, ou um
  backend próprio com cache, se o número de símbolos crescer.
