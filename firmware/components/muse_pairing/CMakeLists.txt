idf_component_register(SRCS link_pairing.c pairing_transcript.c pairing_signer_policy.c
    INCLUDE_DIRS "." PRIV_REQUIRES mbedtls json esp_timer efuse)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH=0)
