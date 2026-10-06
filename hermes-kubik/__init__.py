"""Kubik runs inside Hermes Gateway; no companion daemon or provider credentials on ESP32."""

def register(ctx):
    from .adapter import KubikAdapter
    ctx.register_platform(
        name="kubik", label="Kubik", adapter_factory=KubikAdapter,
        check_fn=lambda: True, validate_config=lambda config: True,
        is_connected=lambda config: bool(config.enabled), required_env=[],
        install_hint="Enable the Kubik platform in Hermes gateway configuration.",
        allowed_users_env="KUBIK_ALLOWED_USERS", allow_all_env="KUBIK_ALLOW_ALL_USERS",
        max_message_length=1000, allow_update_command=False,
        platform_hint="You are speaking to a small desk companion. Give concise plain-text replies.",
    )
