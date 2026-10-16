"""Kubik is a native Hermes Gateway platform, not a companion agent or daemon."""


def register(ctx):
    from .runtime import dependencies_available

    def create_adapter(config):
        from .adapter import KubikAdapter
        return KubikAdapter(config)

    ctx.register_platform(
        name='kubik', label='Kubik', adapter_factory=create_adapter,
        check_fn=dependencies_available,
        is_connected=lambda config: bool(config.enabled), required_env=[],
        install_hint='Install hermes-kubik/requirements.txt in the Hermes environment, then run tools/install-hermes.py.',
        allowed_users_env='KUBIK_ALLOWED_USERS', allow_all_env='KUBIK_ALLOW_ALL_USERS',
        max_message_length=1000, allow_update_command=False, display_tier='minimal',
        platform_hint='You are speaking to a small desk companion. Give concise plain-text replies.',
    )
