# Подключение к Claude, ChatGPT и Codex

ESP32 MCP запускается на **вашем компьютере** командой `server.py` из корня
репозитория. Компьютер выполняет MCP-протокол и обращается к плате; сама плата
не является публичным облачным MCP-сервером. Базовый режим не требует ключа
OpenAI/Groq: модель и её авторизация принадлежат выбранному клиенту.

## Сначала подготовьте плату и Python

1. Установите [прошивку своего профиля](INSTALL_BIN_RU.md).
2. Настройте сеть и токены через `python scripts/configure_device.py --port COM5`.
3. Создайте окружение Python в скачанном проекте и установите `requirements.txt`.
4. Найдите IP платы в Serial Monitor. Токен платы находится в вашем приватном
   `.esp_mcp/device-settings.json`. Не отправляйте этот файл в чат или GitHub.
5. Выберите прямую связь в одной сети либо [relay между сетями](MCP_SETUP_RU.md).

Пример рабочего каталога далее — `C:/projects/esp32-mcp`. Замените его своим путём.
`192.0.2.1` — адрес из документационного диапазона, замените его IP платы.
Токены в примерах — заглушки; реальные значения вводите только локально.

```powershell
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r requirements.txt
$env:ESP_MCP_CONNECTION = 'direct'
$env:ESP_MCP_BOARD_URL = 'http://192.0.2.1:8765'
$env:ESP_MCP_BOARD_TOKEN = 'REPLACE_WITH_BOARD_TOKEN'
.venv/Scripts/python.exe server.py --check
```

На Linux/macOS Python окружения находится в `.venv/bin/python`; последовательность
та же, переменные задаются через `export NAME=value`. `--check` проверяет настройки,
а фактическую связь проверяет инструмент `ping_device` после подключения клиента.

## Claude Desktop: локальный STDIO

Откройте настройки Claude Desktop и раздел локальных MCP-серверов/Developer.
В варианте с JSON-конфигурацией откройте `claude_desktop_config.json` через Edit Config.
На Windows файл обычно расположен в `%APPDATA%/Claude/`, на macOS —
`~/Library/Application Support/Claude/`. Пункты меню отличаются между версиями;
[официальная справка Claude](https://support.claude.com/en/articles/10949351-getting-started-with-local-mcp-servers-on-claude-desktop)
объясняет актуальный раздел локальных серверов и просмотр логов.

Добавьте запись из [готового шаблона](../config/claude.example.json):

```json
{
  "mcpServers": {
    "esp32": {
      "command": "C:/projects/esp32-mcp/.venv/Scripts/python.exe",
      "args": ["C:/projects/esp32-mcp/server.py", "--profile", "generic"],
      "env": {
        "ESP_MCP_CONNECTION": "direct",
        "ESP_MCP_BOARD_URL": "http://192.0.2.1:8765",
        "ESP_MCP_BOARD_TOKEN": "REPLACE_WITH_BOARD_TOKEN",
        "ESP_MCP_VOICE_GATEWAY_PORT": "8777"
      }
    }
  }
}
```

Если в файле уже есть серверы, добавьте `esp32` внутрь существующего `mcpServers`.
Для UNIHIKER K10 замените `generic` на `k10`; есть отдельный
[K10-шаблон](../config/k10.claude.example.json). Полностью закройте Claude Desktop
и запустите снова. Убедитесь, что появились инструменты ESP32, и попросите:

> Используй ping_device, затем hardware_capabilities. Не меняй GPIO до проверки распиновки.

Claude Code также умеет запускать STDIO MCP. Если он установлен, используйте
`claude mcp add-json esp32` с JSON-содержимым внутренней записи `esp32` из шаблона;
точные аргументы вашей версии проверьте командой `claude mcp add-json --help`.
Это отдельная настройка от Claude Desktop. Облачный Claude не читает локальный
JSON автоматически; для удалённого подключения нужен поддерживаемый серверный
транспорт и отдельное безопасное развёртывание.

Для фото/видео и голоса K10 настройте [обратную связь платы с gateway](MCP_SETUP_RU.md):
в обоих K10-шаблонах нужен также `ESP_MCP_VOICE_GATEWAY_TOKEN`, совпадающий с платой.
Рекомендуемый relay требует запущенных gateway и cloudflared; direct/IP соединения
с командами недостаточно для доставки медиа. Токены управления и загрузок различны.

## ChatGPT Desktop: локальный MCP

В версиях с локальным MCP откройте Settings → MCP servers → Add server.
Выберите **STDIO** и задайте:

| Поле | Значение |
| --- | --- |
| Name | esp32 |
| Command | абсолютный путь к Python из `.venv` |
| Arguments | абсолютный путь к корневому `server.py`; для K10 добавьте `--profile k10` |
| Environment | те же четыре переменные из JSON выше |

Сохраните настройки и перезапустите сервер. Проверьте список MCP через `/mcp`,
затем вызовите `ping_device`. Если локальный MCP недоступен в вашей версии или
рабочем пространстве, используйте подходящий удалённый механизм. См.
[официальную инструкцию OpenAI](https://learn.chatgpt.com/docs/extend/mcp).

## ChatGPT в браузере: Secure MCP Tunnel

Браузерный ChatGPT не запускает локальный Python самостоятельно. Официальный
[Secure MCP Tunnel](https://developers.openai.com/api/docs/guides/secure-mcp-tunnels)
может передавать STDIO к локальному серверу. Для него отдельно нужны доступ
к созданию/использованию туннеля, `tunnel_id` и runtime API key OpenAI.

Установите официальный `tunnel-client` по ссылке в документации. В том же терминале
задайте переменные ESP32 из примера выше и `CONTROL_PLANE_API_KEY` приватным способом.
Затем, подставив собственные пути и tunnel ID:

```powershell
tunnel-client help quickstart
tunnel-client init --sample sample_mcp_stdio_local --profile esp32 --tunnel-id REPLACE_WITH_TUNNEL_ID --mcp-command "C:/projects/esp32-mcp/.venv/Scripts/python.exe C:/projects/esp32-mcp/server.py"
tunnel-client doctor --profile esp32 --explain
tunnel-client run --profile esp32
```

В ChatGPT Plugins добавьте custom MCP server с типом Connection: Tunnel и выберите
этот туннель. Свяжите его с нужным ChatGPT workspace в настройках OpenAI Platform.
Держите `tunnel-client` запущенным. Проверка завершается успешным `ping_device`.
Если нужных пунктов нет, проверьте доступ организации/версии продукта по официальной
инструкции. Этот облачный сценарий не был проверен с пользовательским аккаунтом автора.

**Это другой туннель:** `cloudflared` в проекте соединяет плату с ноутбуком по WSS.
Он не превращает `/health` или `/board/ws` в MCP endpoint для ChatGPT. Не указывайте
эти адреса как Streamable HTTP MCP URL. Для Secure MCP Tunnel используйте STDIO
команду корневого `server.py`, как показано выше.

## Codex Desktop / IDE / CLI

Используйте локальную STDIO-запись из [config/mcp.example.toml](../config/mcp.example.toml)
в `~/.codex/config.toml`; для K10 — [отдельный шаблон](../config/k10.mcp.example.toml).
В интерфейсе Codex также можно добавить сервер через настройки MCP, указав Python,
аргументы и env как выше. Перезапустите MCP и проверьте `ping_device`.

Ваш Codex использует вашу авторизацию; доступ к чату, сессии или подписке автора
не передаётся вместе с проектом. Для обычных инструментов отдельный Codex CLI не
нужен, если выбран другой MCP-клиент. CLI требуется опциональному голосовому
кодогенератору K10: установите его, войдите под своей учётной записью, затем
включите `server.py --voice`. Этот режим требует отдельной настройки распознавания
по [основному руководству](MCP_SETUP_RU.md).

## Проверка и первые программы

1. `ping_device` должен вернуть `pong`.
2. `hardware_capabilities` покажет профиль, разрешённые выводы и доступные интерфейсы.
3. `execute_device_code` с `return 6 * 7` проверяет Lua без внешнего оборудования.
4. `list_compatible_devices` показывает реализованные драйверы и статус их тестов.
5. Подключайте модуль только после проверки питания и распиновки. Явно разрешите
   GPIO через `configure_device_pins`; пустой список запрещает внешние выводы.
6. Сохраните программу через `save_device_script` и назначьте короткие команды.
   Для длительного мониторинга задайте `background=true`, используйте паузы,
   `device.publish` и инструменты просмотра/остановки задач.

## Типичные проблемы

| Симптом | Проверка |
| --- | --- |
| Сервер не запускается | Абсолютные пути, зависимости именно в указанном `.venv`, логи клиента |
| Инструментов нет | Перезапуск клиента/MCP; имя и синтаксис конфигурации; разрешения workspace |
| Камеры/микрофона нет в списке | `--profile k10`; эти встроенные устройства не появляются у generic ESP32 |
| `403` / token mismatch | Совпадение приватного BOARD_TOKEN с NVS платы; обновление клиента после USB-настройки |
| IP недоступен | Одна локальная сеть без изоляции либо согласованный relay |
| Порт 8777 занят | Один owning gateway на плату; второй порт 8778 занят его backend |
| `threadId: null` | Обновите host-код; неудачная сессия Codex должна переинициализироваться |
| ИИ обещает неподдерживаемое устройство | Сверьте интерфейсы и конкретный драйвер, не доверяйте одному текстовому обещанию |

Клиентская конфигурация содержит доступ к оборудованию: храните её приватно.
Публичные `.bin`, JSON/TOML-примеры и исходники должны содержать только заглушки.
