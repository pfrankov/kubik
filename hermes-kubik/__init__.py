"""Kubik is a native Hermes Gateway platform, not a companion agent or daemon."""


def register(ctx):
    from .runtime import dependencies_available
    from .printer_tool import register_printer

    from hermes_constants import get_hermes_home
    from .telegram_pairing import TelegramPairing
    pairing = TelegramPairing(get_hermes_home())
    ctx.register_telegram_handler(pairing.install)
    printer = register_printer(ctx)

    def create_adapter(config):
        from .adapter import KubikAdapter
        adapter = KubikAdapter(config)
        adapter.telegram_pairing = pairing
        printer.adapter = adapter
        return adapter

    ctx.register_platform(
        name='kubik', label='Kubik', adapter_factory=create_adapter,
        check_fn=dependencies_available,
        is_connected=lambda config: bool(config.enabled), required_env=[],
        install_hint='Install hermes-kubik/requirements.txt in the Hermes environment, then run tools/install-hermes.py.',
        allowed_users_env='KUBIK_ALLOWED_USERS', allow_all_env='KUBIK_ALLOW_ALL_USERS',
        max_message_length=1000, allow_update_command=False, display_tier='minimal',
        platform_hint=('You are speaking to a small desk companion. Give concise plain-text replies. '
            'Use kubik_printer for physical printing. When you produce useful text or generate or see an '
            'image, decide from the conversation whether a physical print would be useful; you may print '
            'it without an explicit print command. Do not automatically print every response or image. To print an image alone generated '
            'here call kubik_printer with action=print, format=image and image_ref=kubik_image_ref. '
            'If asked to print an image already created in this conversation, use its existing reference; '
            'do not generate another image. If it is unavailable, say so. A displayed image or vision '
            'analysis is not a print. For text and images together, use one kubik_printer document with '
            'ordered text and image blocks; the tool handles pagination. Report the tool result '
            'accurately and never retry an unknown outcome.'),
    )
