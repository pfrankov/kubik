# Nginx Proxy Manager

HTTPS proxy template for `wss://agent.example.com/kubik/v1`. Replace
`agent.example.com` with your domain and `127.0.0.1:18790` with the Kubik
listener address reachable from the NPM container. Container loopback only
works when the listener shares its network namespace.

Include `kubik-http.conf` from `http_top.conf`, and `kubik-server.conf` from
`server_proxy.conf`. Use NPM persistent custom includes; do not edit generated
`proxy_host/*.conf` files.

The proxy terminates TLS and forwards only `/kubik/v1`. Kubik authenticates
the device signature after pairing approval. Keep certificates and keys
outside the repository.

Check the configuration before applying it; replace the container name:

```sh
docker exec nginx-proxy-manager nginx -t
docker exec nginx-proxy-manager nginx -s reload
```

See the [setup guide](../../docs/KIT.ru.md) and
[Cloudflare Tunnel option](../cloudflared/README.md) for other deployments.
