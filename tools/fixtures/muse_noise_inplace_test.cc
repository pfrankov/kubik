// Exact in-place PSA AES-GCM decryption lets C6 reuse its incoming packet.
#include <cassert>
#include <cstdio>
#include <array>
#include <vector>
#include <algorithm>
#include <xplat/noise/core/PsaCryptoBackend.h>
using namespace musegadgets::noise::core;

static void check(PsaCryptoBackend &crypto, size_t size) {
    std::array<uint8_t,32> key{};
    std::array<uint8_t,12> nonce{};
    std::array<uint8_t,8> aad{1,2,3,4,5,6,7,8};
    nonce[0] = size & 255; nonce[1] = size >> 8;
    std::vector<uint8_t> plain(size), packet(size + 16);
    for (size_t i=0; i<size; i++) plain[i] = i % 251;
    assert(crypto.Aes256GcmSeal(ConstByteSpan(key),ConstByteSpan(nonce),ConstByteSpan(aad),
        ConstByteSpan(plain.data(),size),ByteSpan(packet.data(),packet.size())).ok());
    auto damaged = packet; damaged.back() ^= 1;
    assert(crypto.Aes256GcmOpen(ConstByteSpan(key),ConstByteSpan(nonce),ConstByteSpan(aad),
        ConstByteSpan(packet.data(),packet.size()),ByteSpan(packet.data(),size)).ok());
    assert(std::equal(plain.begin(),plain.end(),packet.begin()));
    assert(!crypto.Aes256GcmOpen(ConstByteSpan(key),ConstByteSpan(nonce),ConstByteSpan(aad),
        ConstByteSpan(damaged.data(),damaged.size()),ByteSpan(damaged.data(),size)).ok());
    assert(std::all_of(damaged.begin(),damaged.begin()+size,[](uint8_t value){return value==0;}));
}
int main() {
    PsaCryptoBackend crypto;
    for (size_t size : {size_t(1),size_t(16),size_t(255),size_t(2048),size_t(16384)}) check(crypto,size);
    puts("muse: real PSA in-place decrypt, 16 KiB packets and authentication-failure zeroing passed");
}
