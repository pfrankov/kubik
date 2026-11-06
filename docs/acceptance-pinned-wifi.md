# Физический голосовой стенд с сохранённым Wi-Fi адресом

Эти режимы проверяют настоящий Кубик по Wi-Fi/TLS с локальными голосовыми
провайдерами. Адрес, сети, ключ сопряжения и TLS pin устройства не меняются.
Проверка через USB/SSH не заменяет эту проверку доставки по радио.

Сначала подготовьте зависимости, сборку стенда, диагностический USB-контроль
и существующий TLS JSON с полями `key`/`cert`. Ключ остаётся на своём сервере;
его нельзя переносить на Mac ради теста. Запускайте стенд на сервере этого ключа.
Проверьте отсутствие разговора на устройстве и активной работы Gateway.
Только затем остановите рабочий listener и дождитесь `online=false`, `via=none`.
Мост диагностики должен сохранять `/config`, не перехватывая аудиомаршрут.

`KUBIK_SAVED_ENDPOINT` ниже — точный сохранённый `kubik://host:port` из `info`;
`KUBIK_TLS_FILE` — путь к исходному TLS JSON на этом сервере.

## OpenClaw / S3

```sh
KUBIK_CONTROL_URL=http://127.0.0.1:18791 \
KUBIK_TEST_TLS_IDENTITY="$KUBIK_TLS_FILE" \
KUBIK_TEST_PINNED_ENDPOINT="$KUBIK_SAVED_ENDPOINT" \
node tools/test-device-native-voice.mjs --wifi --audio-profile --paced
```

Стенд проверяет точное совпадение URL, наличие pin и явного порта. Он слушает
этот порт на `0.0.0.0` через ограниченный production TLS listener; устройство
проходит обычную проверку подписи и allowlist. `--barge` добавляет перебивание;
`--echo` измеряет эхо. Без `--audio-profile` сохраняется проверка кадров Tess.

## Hermes / C6

`HERMES_PYTHON` — подготовленный Python 3.14 с зависимостями плагина;
`HERMES_SOURCE` — исходники точной версии из `tools/hermes_native/upstream.json`.
`KUBIK_SAVED_PORT` должен совпадать с портом сохранённого адреса.

```sh
"$HERMES_PYTHON" tools/hermes_native/device.py --source "$HERMES_SOURCE" \
  --tls-identity "$KUBIK_TLS_FILE" --control http://127.0.0.1:18792 \
  --wifi --endpoint "$KUBIK_SAVED_ENDPOINT" --port "$KUBIK_SAVED_PORT" \
  --evidence /path/to/new-private-evidence
```

Настоящие Gateway/AIAgent работают во временном HOME; внешние соединения
блокируются, модели доступны только через loopback; требуется ERLE >10 дБ. `cleanup.json` сохраняет
исходную ошибку и результат восстановления; `send-timing.json` — времена пакетов.
Оба стенда восстанавливают режим до закрытия тестового соединения, не вызывают
Wi-Fi pause и не управляют production-службой. Обязательно в `finally` внешнего
runner возобновите рабочую службу и проверьте обычное подключение устройства,
его режим и сохранённые настройки — также после ошибки или таймаута стенда.
