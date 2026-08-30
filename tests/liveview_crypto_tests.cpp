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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "liveview/liveview_crypto.h"

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static std::string HexDigest(const void* data, size_t size)
{
    uint8_t digest[LIVEVIEW_SHA1_DIGEST_SIZE];
    liveview_sha1(data, size, digest);

    std::string hex;
    char byte[3];

    for (int i = 0; i < LIVEVIEW_SHA1_DIGEST_SIZE; i++)
    {
        snprintf(byte, sizeof(byte), "%02x", digest[i]);
        hex += byte;
    }

    return hex;
}

static std::string HexDigest(const std::string& text)
{
    return HexDigest(text.data(), text.size());
}

static std::string Base64(const void* data, size_t size)
{
    std::vector<char> out(liveview_base64_encoded_size(size) + 1, '\0');
    Check(liveview_base64_encode(data, size, &out[0], out.size()),
        "base64 encoding succeeds with a correctly sized buffer");
    return std::string(&out[0]);
}

static std::string Base64(const std::string& text)
{
    return Base64(text.data(), text.size());
}

static void CheckDigest(const std::string& input, const char* expected, const char* message)
{
    std::string actual = HexDigest(input);

    if (actual != expected)
    {
        fprintf(stderr, "FAIL: %s (expected %s, got %s)\n", message, expected, actual.c_str());
        exit(1);
    }
}

static void CheckBase64(const std::string& input, const char* expected, const char* message)
{
    std::string actual = Base64(input);

    if (actual != expected)
    {
        fprintf(stderr, "FAIL: %s (expected %s, got %s)\n", message, expected, actual.c_str());
        exit(1);
    }
}

static void TestSha1ReferenceVectors()
{
    CheckDigest("", "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "SHA-1 of the empty input matches the reference vector");
    CheckDigest("abc", "a9993e364706816aba3e25717850c26c9cd0d89d",
        "SHA-1 of a short input matches the reference vector");
    CheckDigest("The quick brown fox jumps over the lazy dog",
        "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
        "SHA-1 of a 43 byte input matches the reference vector");
    CheckDigest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "SHA-1 of a 56 byte input matches the reference vector");
    CheckDigest("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
        "a49b2446a02c645bf419f995b67091253a04a259",
        "SHA-1 of a 112 byte multi block input matches the reference vector");
    CheckDigest(std::string(1000000, 'a'), "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "SHA-1 of one million 'a' characters matches the reference vector");

    uint8_t digest[LIVEVIEW_SHA1_DIGEST_SIZE];
    memset(digest, 0xAB, sizeof(digest));
    liveview_sha1(NULL, 0, digest);
    Check(HexDigest(NULL, 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "SHA-1 of a null buffer of size zero equals the empty digest");
}

static void TestSha1BlockBoundaries()
{
    struct BoundaryVector
    {
        size_t length;
        const char* expected;
    };

    static const BoundaryVector k_vectors[] = {
        { 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a" },
        { 56, "c2db330f6083854c99d4b5bfb6e8f29f201be699" },
        { 63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5" },
        { 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d" },
        { 65, "11655326c708d70319be2610e8a57d9a5b959d3b" },
        { 119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56" },
        { 120, "f34c1488385346a55709ba056ddd08280dd4c6d6" },
        { 127, "89d95fa32ed44a7c610b7ee38517ddf57e0bb975" },
        { 128, "ad5b3fdbcb526778c2839d2f151ea753995e26a0" }
    };

    for (size_t i = 0; i < sizeof(k_vectors) / sizeof(k_vectors[0]); i++)
    {
        std::string input(k_vectors[i].length, 'a');
        CheckDigest(input, k_vectors[i].expected,
            "SHA-1 around a padding block boundary matches the reference vector");
    }
}

static void TestBase64ReferenceVectors()
{
    CheckBase64("", "", "base64 of the empty input is empty");
    CheckBase64("f", "Zg==", "base64 pads an input of length mod 3 == 1");
    CheckBase64("fo", "Zm8=", "base64 pads an input of length mod 3 == 2");
    CheckBase64("foo", "Zm9v", "base64 of an input of length mod 3 == 0 is unpadded");
    CheckBase64("foob", "Zm9vYg==", "base64 pads a four byte input");
    CheckBase64("fooba", "Zm9vYmE=", "base64 pads a five byte input");
    CheckBase64("foobar", "Zm9vYmFy", "base64 of a six byte input is unpadded");

    static const uint8_t k_high_bytes[] = { 0xFB, 0xFF, 0xBF };
    Check(Base64(k_high_bytes, sizeof(k_high_bytes)) == "+/+/",
        "base64 encodes bytes that map to the last two alphabet characters");

    static const uint8_t k_zero_bytes[] = { 0x00, 0x00, 0x00, 0x00 };
    Check(Base64(k_zero_bytes, sizeof(k_zero_bytes)) == "AAAAAA==",
        "base64 encodes zero bytes");

    Check(liveview_base64_encoded_size(0) == 0, "encoded size of zero bytes is zero");
    Check(liveview_base64_encoded_size(1) == 4, "encoded size of one byte is four");
    Check(liveview_base64_encoded_size(2) == 4, "encoded size of two bytes is four");
    Check(liveview_base64_encoded_size(3) == 4, "encoded size of three bytes is four");
    Check(liveview_base64_encoded_size(4) == 8, "encoded size of four bytes is eight");
}

static void TestBase64Capacity()
{
    char out[8];
    memset(out, 'x', sizeof(out));

    Check(!liveview_base64_encode("foo", 3, out, 4),
        "base64 rejects a buffer without room for the terminator");
    Check(out[0] == 'x' && out[3] == 'x',
        "a rejected base64 encoding leaves the output buffer untouched");
    Check(!liveview_base64_encode("foo", 3, NULL, 16),
        "base64 rejects a null output buffer");
    Check(liveview_base64_encode("foo", 3, out, 5),
        "base64 accepts a buffer with room for the terminator");
    Check(strcmp(out, "Zm9v") == 0, "base64 terminates its output");
}

static void TestWebSocketAcceptVector()
{
    // RFC 6455 section 1.3: the client key concatenated with the protocol GUID,
    // hashed with SHA-1 and base64 encoded, yields the Sec-WebSocket-Accept value.
    std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + guid;

    uint8_t digest[LIVEVIEW_SHA1_DIGEST_SIZE];
    liveview_sha1(combined.data(), combined.size(), digest);

    std::string accept = Base64(digest, sizeof(digest));

    if (accept != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
    {
        fprintf(stderr, "FAIL: RFC 6455 accept value (got %s)\n", accept.c_str());
        exit(1);
    }
}

static void TestThreadSafety()
{
    const int k_threads = 8;
    std::string input(4096, 'z');
    std::string expected_digest = HexDigest(input);
    std::string expected_base64 = Base64(input);

    std::vector<std::string> digests(k_threads);
    std::vector<std::string> encodings(k_threads);
    std::vector<std::thread> workers;

    for (int i = 0; i < k_threads; i++)
    {
        workers.push_back(std::thread([&digests, &encodings, &input, i]()
        {
            for (int round = 0; round < 50; round++)
            {
                digests[i] = HexDigest(input);
                encodings[i] = Base64(input);
            }
        }));
    }

    for (size_t i = 0; i < workers.size(); i++)
        workers[i].join();

    for (int i = 0; i < k_threads; i++)
    {
        Check(digests[i] == expected_digest,
            "concurrent SHA-1 calls produce the single threaded digest");
        Check(encodings[i] == expected_base64,
            "concurrent base64 calls produce the single threaded encoding");
    }
}

int main()
{
    TestSha1ReferenceVectors();
    TestSha1BlockBoundaries();
    TestBase64ReferenceVectors();
    TestBase64Capacity();
    TestWebSocketAcceptVector();
    TestThreadSafety();

    printf("All live view crypto tests passed\n");

    return 0;
}
