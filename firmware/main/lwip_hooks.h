#pragma once
// ESP-IDF's documented lwIP hook runs before tcp_out.c's allocator policy.
// Wi-Fi already copies chained pbufs at its transmit boundary. Allow NODELAY
// records to allocate only their actual size, leaving SRAM for incoming ACKs.
#ifdef KUBIK_TCP_OUT_ALLOCATIONS
#if LWIP_NETIF_TX_SINGLE_PBUF != 1
#error "Review the pinned lwIP single-pbuf allocation policy"
#endif
#undef LWIP_NETIF_TX_SINGLE_PBUF
#define LWIP_NETIF_TX_SINGLE_PBUF 0

#endif
