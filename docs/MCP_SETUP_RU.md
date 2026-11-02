# Установка ESP32 MCP

Отдельная пошаговая инструкция для [Claude, ChatGPT Desktop/Web и Codex](AI_CLIENTS_RU.md)
включает локальный STDIO, проверку инструментов и официальный Secure MCP Tunnel.

## 1. Что потребуется

Python 3.11/3.12, VS Code с PlatformIO, USB-кабель с передачей данных, ESP32/ESP32-S3/ESP32-C3 либо UNIHIKER K10, совместимая Wi-Fi сеть. Для обычного MCP не нужны Groq, OpenAI API или отдельный Codex CLI: авторизация модели настраивается в вашем MCP-клиенте.

Для дополнительного голосового режима K10 нужны локальный Whisper либо Groq, а для генерации новых программ — установленный и авторизованный Codex CLI. Это отдельная сессия, не текущий чат приложения.

## 2. Локальные настройки платы

Скопируйте `include/board_config.example.h` в `include/board_config.h`. Файл исключён из публикации. Заполните:

| Настройка | Значение |
| --- | --- |
| WIFI_SSID / WIFI_PASSWORD | Ваша сеть 2,4 ГГц и пароль |
| BOARD_TOKEN | Случайный токен минимум 24 символа; совпадает с ESP_MCP_BOARD_TOKEN |
| VOICE_GATEWAY_TOKEN | Другой случайный токен; совпадает с ESP_MCP_VOICE_GATEWAY_TOKEN |
| VOICE_GATEWAY_URL | Доступный плате адрес gateway либо пусто при USB-настройке туннеля |
| MCP_ALLOWED_PINS | CSV безопасных внешних GPIO; пусто запрещает доступ |
| MAIN_PORT / RESCUE_PORT | Обычно 8765 / 8766 |

Генерируйте токены локально, например `python -c "import secrets; print(secrets.token_hex(32))"`. Не помещайте API-ключи провайдеров в прошивку. Проверьте назначение каждого GPIO по схеме вашей платы: нельзя отдавать скриптам выводы flash, USB, встроенной камеры/экрана/микрофона.

## 3. Прошивка

Откройте эту папку в новом окне VS Code. В PlatformIO выберите `esp32dev`, `esp32s3`, `esp32c3`, `esp32c6`, `esp32s2`, `esp32wrover` или **`k10`**. Соберите Build, затем осознанно запустите Upload на правильный USB-порт. Нельзя загружать default esp32dev на K10.

K10: обычное `k10` сохраняет раздел ASR. Для отсутствующей английской модели один раз используйте `k10_model_en`, затем вернитесь на `k10`. Это vendor-модель; её локальная wake-фраза не меняется через MCP. После запуска проверьте Serial Monitor 115200: отсутствие перезагрузок, IP, профиль, готовность. Generic-платы без дисплея выводят этапы в Serial.

## 4. Host-зависимости

```powershell
python -m venv .venv
.venv/Scripts/python.exe -m pip install -r requirements.txt
```

Не используйте Python из чужого проекта. Зависимости MCP, WebSocket, RestrictedPython и медиа нужны и без голосового режима. `requirements-local-stt.txt` устанавливайте дополнительно только для локальной транскрипции.

## 5. Подключение по локальной сети

Задайте переменные окружения в конфигурации MCP-клиента:

```powershell
$env:ESP_MCP_CONNECTION = 'direct'
$env:ESP_MCP_BOARD_URL = 'http://192.0.2.1:8765'
$env:ESP_MCP_BOARD_TOKEN = 'REPLACE_WITH_BOARD_TOKEN'
$env:ESP_MCP_VOICE_GATEWAY_PORT = '8777'
.venv/Scripts/python.exe server.py --check
```

IP выше — пример, замените фактическим. Проверка `--check` проверяет формат, а не связь. Для K10 задайте `--profile k10` в args либо используйте отдельные K10-примеры в config/. По умолчанию инструменты запускаются с минимальным generic-профилем без камеры/голоса. Для реального контроля вызовите MCP `ping_device`, затем `hardware_capabilities`. Школьная сеть может изолировать клиентов; не отключайте защиту компьютера ради этого.

## 6. Регистрация в Codex или Claude

Используйте [TOML](../config/mcp.example.toml) или [JSON](../config/claude.example.json). Замените путь к Python, путь к **корневому server.py**, адрес и токены. TOML-секции должны называться одинаково: `mcp_servers.esp32` и `mcp_servers.esp32.env`. Перезапустите MCP-клиент. Внутренний `mcp/server.py` не является публичной точкой входа.

Если другой MCP уже слушает 8777, остановите его либо выберите согласованную пару портов. Второй порт N+1 — loopback backend. Один gateway обслуживает одну плату; не запускайте независимые экземпляры с разными токенами на одной паре портов.

## 7. Разные сети / изоляция Wi-Fi

Для **K10 с передачей фото, видео и голоса** рекомендуем этот режим даже в одной
сети: загрузка медиа использует исходящий WebSocket платы. В конфигурацию клиента
перенесите **оба** токена из приватного device-settings.json: BOARD_TOKEN управляет
командами, VOICE_GATEWAY_TOKEN аутентифицирует загрузки. K10-шаблоны содержат оба поля.
Один только direct/IP доступ к командам не настраивает обратную передачу медиа.

Установите `cloudflared`, разрешённый политикой вашей сети. Плата инициирует исходящее TLS/WSS-соединение; входящий порт платы не публикуется.

```powershell
$env:ESP_MCP_CONNECTION = 'relay'
$env:ESP_MCP_VOICE_GATEWAY_TOKEN = 'REPLACE_WITH_GATEWAY_TOKEN'
.venv/Scripts/python.exe server.py --gateway-only
```

В другом терминале, с теми же настройками и закрытым Serial Monitor:

```powershell
.venv/Scripts/python.exe mcp/start_quick_tunnel.py --port COM5 --gateway-port 8777 --protocol http2
```

Замените COM5 своим портом. Помощник передаёт время и временный HTTPS URL по USB командами `ESP_MCP_TIME` и `ESP_MCP_RELAY_URL`. Адрес хранится в NVS. Держите процесс туннеля включённым; после перезапуска адрес обычно меняется, его нужно передать снова. Не отключайте TLS-проверку. Без доступа сети к туннелю этот вариант работать не будет.

Помощник проверяет готовность cloudflared каждые 10 секунд. Если соединение с
Cloudflare не восстанавливается дольше 45 секунд, он пересоздаёт туннель и передаёт
плате новый URL. Для автоматического обновления нужен подключённый USB и свободный
COM-порт: закройте Serial Monitor. При занятом порте помощник повторяет настройку.
Краткие обрывы не меняют адрес, аппаратные команды не переигрываются.

Для K10 добавьте `--profile k10` к запуску owning gateway и к args MCP-клиента.
Клиент должен использовать `ESP_MCP_CONNECTION=relay` и те же токены и порт 8777;
он присоединится к запущенному gateway. Сначала проверяйте `ping_device`, затем
`take_photo`. Не запускайте параллельно два независимых owning gateway.

Для голосового owning gateway используйте `server.py --gateway-only --voice` с
настройками распознавания ниже вместо tools-only процесса. Для локального STT
добавьте `--local-stt`; для Groq задайте ключ только в окружении ноутбука.

В клиенте задайте те же relay-переменные. `server.py` подключится к уже запущенному owning gateway. Для LAN загрузки фото/видео также требуются URL gateway, совпадающий upload-токен и доступ с платы к ноутбуку; одного прямого MCP HTTP недостаточно.

## 8. Первая проверка Lua и сохранения

Через MCP вызовите `execute_device_code` с `source="return 6 * 7"`, затем `device_code_status` с выданным job_id. Ожидаемый результат — 42. Сохраните эту программу через `save_device_script`, указав name, voice_phrase и short_commands. Проверьте installation, затем `list_device_scripts`, `read_device_script` и `run_device_script`.

Сохранённый на плате исходник можно прочитать с другого ноутбука при наличии доступа и токена. Локальные файлы остаются в `skills/lua`. Задания после reset не возобновляются, навыки автоматически не запускаются. [API и лимиты Lua](NATIVE_LUA.md).

JSON сохраняется через `save_device_skill`, композиции — через `run_composed_skill`. Python использует `save_python_skill` и отдельный процесс ноутбука. У всех навыков сохраняйте короткую команду и алиасы. [Сравнение исполнителей](EXECUTION_MODES_RU.md).

## 9. Дополнительный голос K10

Groq: задайте `GROQ_API_KEY` в окружении ноутбука, совпадающий gateway-токен, затем `server.py --voice`. Локально: установите `requirements-local-stt.txt`, запустите `server.py --voice --local-stt`. Первый запуск скачивает веса, после прогрева проверьте время и качество на своём оборудовании. Не запускайте новый voice gateway поверх tools-only gateway: сначала остановите владельца портов.

A → речь → A отправляет один WAV. Транскрипт выводится в консоль. Короткая фраза существующего навыка идёт через каталог; новые программы создаёт настроенный Codex. B свободна. Для новых программ выбранная модель определяется ESP_MCP_CODEX_MODEL/настройками CLI, а не названием текущего чата. [Речь](SPEECH.md).

## 10. Проверка перед публикацией

```powershell
python -m pip install -r requirements-dev.txt
python -m unittest discover -s tests -t . -v
python scripts/check_repository.py
python scripts/package_source.py
```

Публикуйте проверенный source archive либо явный список исходников. Не публикуйте board_config.h, .esp_mcp, .pio, venv, captures, logs, .env, compile_commands.json и прошитые бинарники. Сканирование не заменяет просмотр diff/архива. Ограничения и обработка данных: [SECURITY](../SECURITY.md).
