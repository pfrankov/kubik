# Cloudflare Tunnel для Кубика

Нужен, если у вас **свой домен** в Cloudflare, а OpenClaw и Кубик находятся в разных сетях (или OpenClaw стоит за NAT без публичного адреса). Без домена используйте другие варианты из «Ваш случай» в `README.md` комплекта: в одной сети ничего не нужно, для VPS с публичным IP есть `listen.public`.

**Постоянный Tunnel из комплекта работает на хосте OpenClaw под Linux с systemd.** Прошивать Кубик можно с macOS, Windows или Linux. Плагин можно запускать и на другом хосте OpenClaw, но именованный Tunnel этот комплект там не устанавливает. Хост OpenClaw с macOS или Windows и своим доменом: вручную и официально не поддерживается. Комплект там не проверялся; у Cloudflare есть собственная установка службы (`cloudflared service install`), после неё шаги 3–7 те же, но токен остаётся в параметрах службы, а не в файле с правами `0600`.

Tunnel направляется на **порт OpenClaw Gateway** (по умолчанию 18789, параметр `gateway.port`) с правилом только для пути `/kubik/v1`: именно на этом порту плагин регистрирует маршрут устройства. Остальной Gateway и Control UI наружу не публикуются. Cloudflare Tunnel сам открывает исходящее соединение с хоста OpenClaw: не пробрасывайте порты 18789 и 18790 на роутере.

## Где что выполняется

- **Хост OpenClaw** (компьютер, на котором работает OpenClaw): установка `cloudflared`, установщик Tunnel, команды `openclaw`.
- **Панель Cloudflare** (в браузере): создание Tunnel, маршрут, WebSockets.
- **Любой компьютер с интернетом и Node.js**: проверка адреса `probe.mjs`.
- **Телефон**: страница настройки Кубика.

## Постоянный Tunnel

1. *Хост OpenClaw, браузер.* Установите `cloudflared` 2025.4.0 или новее по [инструкции Cloudflare для Linux](https://developers.cloudflare.com/tunnel/features/locally-managed-tunnels/create-local-tunnel/#1-download-and-install-cloudflared). Добавьте домен в Cloudflare и создайте удалённо управляемый именованный Tunnel: **Networking → Tunnels**.
2. *Хост OpenClaw.* Скопируйте токен Tunnel из панели. В распакованной папке комплекта выполните:

   ```bash
   sudo bash deploy/cloudflared/install-systemd.sh
   ```

   Токен вставляйте только в скрытый запрос установщика. Не запускайте и не сохраняйте команду службы с токеном из панели Cloudflare. Установщик создаёт системного пользователя `kubik-cloudflared` без входа, сохраняет токен в `/etc/kubik-cloudflared/token` с правами `0600` и ставит службу `/etc/systemd/system/kubik-cloudflared.service`. В аргументах службы только путь к файлу токена; неудачный запуск повторяется не чаще раза в десять секунд без конечного лимита. Проверьте службу: `sudo systemctl status --no-pager kubik-cloudflared.service`, затем убедитесь, что коннектор в Cloudflare **Healthy**: запущенный процесс ещё не доказывает подключения.
3. *Панель Cloudflare.* Добавьте в Tunnel один **Published application**:
   - Hostname: `kubik.example.com` (имя в вашей зоне Cloudflare);
   - Path: `/kubik/v1` (только этот путь, больше ничего);
   - Service: `http://127.0.0.1:18789` (порт Gateway, `gateway.port`).
   Источник получит исходный путь `/kubik/v1`.
4. *Панель Cloudflare.* В разделе **Network** включите **WebSockets**.
5. *Хост OpenClaw.* Чтобы Gateway видел настоящие адреса клиентов, доверьте ему локальный коннектор и перезапустите Gateway:

   ```bash
   openclaw config set gateway.trustedProxies '["127.0.0.1"]' --strict-json
   openclaw gateway restart
   ```

   `openclaw channels status --probe` должен показать `route /kubik/v1`. Маршрут проверяет ключ устройства сам (`auth: "plugin"`), токен Gateway Кубику не нужен. `gateway.trustedProxies` должен содержать TCP-адрес, с которого соединяется коннектор. Без этой настройки все удалённые устройства делят лимиты одновременных рукопожатий и ожидающих запросов на адрес коннектора.

   **Имя хоста в подписи.** Кубик с прошивкой 0.6.1 и новее подписывает вход вместе с именем, к которому подключился (`kubik.example.com`), а плагин сверяет его с заголовком `Host` запроса. Так чужой сервер с настоящим сертификатом не может передать рукопожатие вашему OpenClaw. По умолчанию Cloudflare Tunnel передаёт в `Host` публичное имя: не задавайте в приложении Tunnel параметр **HTTP Host Header** (`httpHostHeader`). Если `Host` всё же заменяется на адрес источника (`127.0.0.1:18789`), плагин возьмёт публичное имя из `X-Forwarded-Host`, но только от адреса из `gateway.trustedProxies` и только пока в `Host` адрес или имя без точки; заголовок `X-Forwarded-Host` прокси в этом случае должен выставлять сам. Признак ошибки: устройство закрывается с `unauthorized`, а в журнале Gateway строка `rejected device … transport binding mismatch … this request arrived on host "…"`: в кавычках имя, с которым сравнивалась подпись. Прошивка 0.6.0 подписывает `ca` без имени: плагин её принимает и пишет предупреждение `signed the legacy binding`.
6. *Любой компьютер с Node.js 24.16 или новее (24.x) либо 26.1 или новее и интернетом.* Проверьте точный адрес до ввода в Кубика:

   ```bash
   node deploy/cloudflared/probe.mjs wss://kubik.example.com/kubik/v1
   ```

   Проверка делает WSS-рукопожатие и получает вызов Kubik v4 с одноразовым публичным ключом. Она не запрашивает сопряжение, не передаёт звук и не доказывает, что ESP32 доверяет сертификату. При ошибке исправьте DNS, TLS, Tunnel или маршрут.
7. *Телефон, страница настройки Кубика.* В поле адреса введите `kubik.example.com` (или полностью `wss://kubik.example.com/kubik/v1`). Код с экрана Кубика одобрите на хосте OpenClaw: `openclaw pairing approve kubik КОД`.

## Что видит Cloudflare

Соединение Кубик → Cloudflare идёт по WSS, коннектор → Cloudflare тоже шифруется. Cloudflare завершает WSS и **может видеть голосовой трафик**. Маршрут публичный: сопряжение по ключу P-256 аутентифицирует устройства, но это не вход Cloudflare Access. Прошивка не отправляет заголовки Cloudflare Access, поэтому не требуйте Access-личность или service token на этом имени. Gateway доверяет заголовкам пересылки только адресам из `gateway.trustedProxies`. Это разделяет лимиты по клиентам; неверная подпись не создаёт постоянного бана адреса или ключа.

## Проверка для разработчиков (без рабочих данных)

Только из исходного репозитория (в ZIP клиента этого скрипта нет): установите `cloudflared`, Node.js 24.16 или новее (24.x) либо 26.1 или новее, выполните `npm install` в `openclaw-kubik/`, затем:

```bash
node deploy/cloudflared/smoke.mjs
```

Проверка открывает короткоживущий публичный Quick Tunnel к локальному WebSocket-моку, который возвращает вызов Kubik v4 только на `/kubik/v1`. Она запускает тот же `probe.mjs`, что и в комплекте, проверяет заголовок с адресом клиента и закрывает оба процесса. Она не проверяет аккаунт Cloudflare, именованный маршрут, DNS и рабочий Gateway. Quick Tunnel нужен только для тестов.

Документация Cloudflare: [установка на Linux](https://developers.cloudflare.com/tunnel/features/locally-managed-tunnels/create-local-tunnel/#1-download-and-install-cloudflared), [создание удалённо управляемого Tunnel](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/get-started/create-remote-tunnel/), [параметры запуска и токен-файл](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/configure-tunnels/run-parameters/), [маршруты опубликованных приложений](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/routing-to-tunnel/), [WebSockets](https://developers.cloudflare.com/network/websockets/), [порты Tunnel для файрвола](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/configure-tunnels/tunnel-with-firewall/).
