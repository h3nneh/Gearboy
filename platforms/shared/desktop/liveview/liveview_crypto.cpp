/*
 * Gearboy - Nintendo Game Boy Emulator
 * Copyright (C) 2012  Ignacio Sanchez

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

#include "liveview_crypto.h"

#include <cstring>

#define LIVEVIEW_SHA1_BLOCK_SIZE 64

struct LiveviewSha1State
{
    uint32_t hash[5];
    uint64_t bit_count;
    uint8_t block[LIVEVIEW_SHA1_BLOCK_SIZE];
    size_t block_size;
};

static const char k_liveview_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t LiveviewSha1Rotate(uint32_t value, unsigned int bits)
{
    return (uint32_t)((value << bits) | (value >> (32u - bits)));
}

static void LiveviewSha1Init(LiveviewSha1State* state)
{
    state->hash[0] = 0x67452301u;
    state->hash[1] = 0xEFCDAB89u;
    state->hash[2] = 0x98BADCFEu;
    state->hash[3] = 0x10325476u;
    state->hash[4] = 0xC3D2E1F0u;
    state->bit_count = 0;
    state->block_size = 0;
}

static void LiveviewSha1Compress(LiveviewSha1State* state, const uint8_t* block)
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)block[(i * 4) + 0] << 24) |
               ((uint32_t)block[(i * 4) + 1] << 16) |
               ((uint32_t)block[(i * 4) + 2] << 8) |
               ((uint32_t)block[(i * 4) + 3]);
    }

    for (int i = 16; i < 80; i++)
    {
        w[i] = LiveviewSha1Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state->hash[0];
    uint32_t b = state->hash[1];
    uint32_t c = state->hash[2];
    uint32_t d = state->hash[3];
    uint32_t e = state->hash[4];

    for (int i = 0; i < 80; i++)
    {
        uint32_t f = 0;
        uint32_t k = 0;

        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        uint32_t temp = LiveviewSha1Rotate(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = LiveviewSha1Rotate(b, 30);
        b = a;
        a = temp;
    }

    state->hash[0] += a;
    state->hash[1] += b;
    state->hash[2] += c;
    state->hash[3] += d;
    state->hash[4] += e;
}

static void LiveviewSha1Update(LiveviewSha1State* state, const uint8_t* data, size_t size)
{
    state->bit_count += (uint64_t)size * 8u;

    size_t offset = 0;

    if (state->block_size > 0)
    {
        size_t missing = (size_t)LIVEVIEW_SHA1_BLOCK_SIZE - state->block_size;
        size_t taken = (size < missing) ? size : missing;
        memcpy(state->block + state->block_size, data, taken);
        state->block_size += taken;
        offset += taken;

        if (state->block_size < (size_t)LIVEVIEW_SHA1_BLOCK_SIZE)
            return;

        LiveviewSha1Compress(state, state->block);
        state->block_size = 0;
    }

    while ((size - offset) >= (size_t)LIVEVIEW_SHA1_BLOCK_SIZE)
    {
        LiveviewSha1Compress(state, data + offset);
        offset += (size_t)LIVEVIEW_SHA1_BLOCK_SIZE;
    }

    size_t remaining = size - offset;

    if (remaining > 0)
    {
        memcpy(state->block, data + offset, remaining);
        state->block_size = remaining;
    }
}

static void LiveviewSha1Finish(LiveviewSha1State* state, uint8_t* digest)
{
    uint64_t bit_count = state->bit_count;
    uint8_t padding_start = 0x80;
    uint8_t padding_zero = 0x00;

    LiveviewSha1Update(state, &padding_start, 1);
    state->bit_count = bit_count;

    while (state->block_size != 56)
    {
        LiveviewSha1Update(state, &padding_zero, 1);
        state->bit_count = bit_count;
    }

    uint8_t length[8];

    for (int i = 0; i < 8; i++)
    {
        length[i] = (uint8_t)((bit_count >> (56 - (i * 8))) & 0xFFu);
    }

    LiveviewSha1Update(state, length, sizeof(length));

    for (int i = 0; i < LIVEVIEW_SHA1_DIGEST_SIZE; i++)
    {
        digest[i] = (uint8_t)((state->hash[i / 4] >> (24 - ((i % 4) * 8))) & 0xFFu);
    }
}

void liveview_sha1(const void* data, size_t size, uint8_t* digest)
{
    if (digest == NULL)
        return;

    LiveviewSha1State state;
    LiveviewSha1Init(&state);

    if (size > 0 && data != NULL)
        LiveviewSha1Update(&state, (const uint8_t*)data, size);

    LiveviewSha1Finish(&state, digest);
}

size_t liveview_base64_encoded_size(size_t size)
{
    return ((size + 2u) / 3u) * 4u;
}

bool liveview_base64_encode(const void* data, size_t size, char* out, size_t out_capacity)
{
    if (out == NULL)
        return false;

    if (size > 0 && data == NULL)
        return false;

    size_t needed = liveview_base64_encoded_size(size) + 1u;

    if (out_capacity < needed)
        return false;

    const uint8_t* bytes = (const uint8_t*)data;
    size_t written = 0;
    size_t offset = 0;

    while ((size - offset) >= 3u)
    {
        uint32_t chunk = ((uint32_t)bytes[offset] << 16) |
                         ((uint32_t)bytes[offset + 1] << 8) |
                         ((uint32_t)bytes[offset + 2]);
        out[written++] = k_liveview_base64_alphabet[(chunk >> 18) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[(chunk >> 12) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[(chunk >> 6) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[chunk & 0x3Fu];
        offset += 3u;
    }

    size_t remaining = size - offset;

    if (remaining == 1u)
    {
        uint32_t chunk = ((uint32_t)bytes[offset] << 16);
        out[written++] = k_liveview_base64_alphabet[(chunk >> 18) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[(chunk >> 12) & 0x3Fu];
        out[written++] = '=';
        out[written++] = '=';
    }
    else if (remaining == 2u)
    {
        uint32_t chunk = ((uint32_t)bytes[offset] << 16) |
                         ((uint32_t)bytes[offset + 1] << 8);
        out[written++] = k_liveview_base64_alphabet[(chunk >> 18) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[(chunk >> 12) & 0x3Fu];
        out[written++] = k_liveview_base64_alphabet[(chunk >> 6) & 0x3Fu];
        out[written++] = '=';
    }

    out[written] = '\0';

    return true;
}
