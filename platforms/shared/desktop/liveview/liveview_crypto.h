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

#ifndef LIVEVIEW_CRYPTO_H
#define LIVEVIEW_CRYPTO_H

#include <cstddef>
#include <stdint.h>

#define LIVEVIEW_SHA1_DIGEST_SIZE 20

// Pure helpers for the live view WebSocket handshake. Both functions are free
// of global state and of dynamic allocation, so they are safe to call from any
// thread at any time.

// Writes the SHA-1 digest of the first size bytes at data into digest, which
// must have room for LIVEVIEW_SHA1_DIGEST_SIZE bytes. A null data pointer is
// only valid together with a size of zero.
void liveview_sha1(const void* data, size_t size, uint8_t* digest);

// Number of base64 characters produced for size input bytes, not counting the
// terminating null character.
size_t liveview_base64_encoded_size(size_t size);

// Encodes the first size bytes at data as base64 into out and terminates it
// with a null character. Returns false and leaves out untouched when out is
// null or out_capacity is smaller than liveview_base64_encoded_size(size) + 1.
bool liveview_base64_encode(const void* data, size_t size, char* out, size_t out_capacity);

#endif /* LIVEVIEW_CRYPTO_H */
