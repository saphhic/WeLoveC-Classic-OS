// Written by [@saphhic](https://github.com/saphhic)
// Date: October 30th 2026
// Description: Implements password hash generation and verification.

#include "password_hash.hpp"

namespace welovec::security::crypto {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct Sha256Context {
    uint32_t state[8];
    uint64_t total_bytes;
    uint32_t block_size;
    uint8_t block[64];
};

uint32_t rotate_right(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << (32 - amount));
}

uint32_t read_be32(const uint8_t *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

void transform(Sha256Context *context, const uint8_t block[64]) {
    uint32_t schedule[64];
    for (uint32_t i = 0; i < 16; ++i) schedule[i] = read_be32(block + i * 4);
    for (uint32_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                            rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
        const uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                            rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + sum1 + choose + kRoundConstants[i] + schedule[i];
        const uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
    secure_zero(schedule, sizeof(schedule));
}

void initialize(Sha256Context *context) {
    static constexpr uint32_t initial_state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    for (uint32_t i = 0; i < 8; ++i) context->state[i] = initial_state[i];
    context->total_bytes = 0;
    context->block_size = 0;
}

void update(Sha256Context *context, const uint8_t *data, uint64_t size) {
    if (!data || !size) return;
    context->total_bytes += size;
    while (size) {
        uint32_t take = 64 - context->block_size;
        if (take > size) take = static_cast<uint32_t>(size);
        for (uint32_t i = 0; i < take; ++i)
            context->block[context->block_size + i] = data[i];
        context->block_size += take;
        data += take;
        size -= take;
        if (context->block_size == 64) {
            transform(context, context->block);
            context->block_size = 0;
        }
    }
}

void finish(Sha256Context *context, uint8_t output[32]) {
    const uint64_t bit_count = context->total_bytes * 8;
    context->block[context->block_size++] = 0x80;
    if (context->block_size > 56) {
        while (context->block_size < 64) context->block[context->block_size++] = 0;
        transform(context, context->block);
        context->block_size = 0;
    }
    while (context->block_size < 56) context->block[context->block_size++] = 0;
    for (uint32_t i = 0; i < 8; ++i)
        context->block[63 - i] = static_cast<uint8_t>(bit_count >> (i * 8));
    transform(context, context->block);
    for (uint32_t i = 0; i < 8; ++i) write_be32(output + i * 4, context->state[i]);
    secure_zero(context, sizeof(*context));
}

void hmac_two_parts(const uint8_t *key, uint64_t key_size,
                    const uint8_t *first, uint64_t first_size,
                    const uint8_t *second, uint64_t second_size,
                    uint8_t output[32]) {
    uint8_t key_block[64]{};
    if (key_size > sizeof(key_block)) {
        sha256(key, key_size, key_block);
        key_size = 32;
    } else {
        for (uint64_t i = 0; i < key_size; ++i) key_block[i] = key[i];
    }

    uint8_t inner_pad[64], outer_pad[64], inner_hash[32];
    for (uint32_t i = 0; i < 64; ++i) {
        inner_pad[i] = key_block[i] ^ 0x36;
        outer_pad[i] = key_block[i] ^ 0x5c;
    }
    Sha256Context context{};
    initialize(&context);
    update(&context, inner_pad, sizeof(inner_pad));
    update(&context, first, first_size);
    update(&context, second, second_size);
    finish(&context, inner_hash);
    initialize(&context);
    update(&context, outer_pad, sizeof(outer_pad));
    update(&context, inner_hash, sizeof(inner_hash));
    finish(&context, output);
    secure_zero(key_block, sizeof(key_block));
    secure_zero(inner_pad, sizeof(inner_pad));
    secure_zero(outer_pad, sizeof(outer_pad));
    secure_zero(inner_hash, sizeof(inner_hash));
}

}

void sha256(const uint8_t *data, uint64_t size, uint8_t output[32]) {
    if (!output) return;
    Sha256Context context{};
    initialize(&context);
    update(&context, data, size);
    finish(&context, output);
}

void hmac_sha256(const uint8_t *key, uint64_t key_size,
                 const uint8_t *data, uint64_t data_size, uint8_t output[32]) {
    if (!output || (!key && key_size) || (!data && data_size)) return;
    hmac_two_parts(key, key_size, data, data_size, nullptr, 0, output);
}

void pbkdf2_sha256(const uint8_t *password, uint64_t password_size,
                   const uint8_t *salt, uint64_t salt_size,
                   uint32_t iterations, uint8_t output[32]) {
    if (!output || (!password && password_size) || (!salt && salt_size) || !iterations) return;
    uint8_t block[4] = {0, 0, 0, 1};
    uint8_t u[32], result[32];
    hmac_two_parts(password, password_size, salt, salt_size, block, sizeof(block), u);
    for (uint32_t i = 0; i < sizeof(result); ++i) result[i] = u[i];
    for (uint32_t round = 1; round < iterations; ++round) {
        hmac_sha256(password, password_size, u, sizeof(u), u);
        for (uint32_t i = 0; i < sizeof(result); ++i) result[i] ^= u[i];
    }
    for (uint32_t i = 0; i < sizeof(result); ++i) output[i] = result[i];
    secure_zero(u, sizeof(u));
    secure_zero(result, sizeof(result));
}

bool constant_time_equal(const uint8_t *left, const uint8_t *right, uint64_t size) {
    if (!left || !right) return false;
    uint8_t difference = 0;
    for (uint64_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

void secure_zero(void *memory, uint64_t size) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(memory);
    while (size--) *bytes++ = 0;
}

}
