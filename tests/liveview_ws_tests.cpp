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
#include <vector>
#include "liveview/liveview_ws.h"

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void CheckEqual(const std::string& actual, const std::string& expected,
    const char* message)
{
    if (actual != expected)
    {
        fprintf(stderr, "FAIL: %s\nexpected: [%s]\nactual:   [%s]\n", message,
            expected.c_str(), actual.c_str());
        exit(1);
    }
}

// Deterministic payload so failures are reproducible.
static std::vector<uint8_t> MakePayload(size_t size)
{
    std::vector<uint8_t> payload(size);
    uint32_t value = 0x12345678u;

    for (size_t i = 0; i < size; i++)
    {
        value = (value * 1103515245u) + 12345u;
        payload[i] = (uint8_t)((value >> 16) & 0xFFu);
    }

    return payload;
}

// Independent client side frame builder: masked, as a browser sends them.
static std::vector<uint8_t> MakeClientFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
    bool fin, const uint8_t mask[4], bool masked)
{
    std::vector<uint8_t> frame;
    frame.push_back((uint8_t)((fin ? 0x80u : 0x00u) | (opcode & 0x0Fu)));

    size_t size = payload.size();
    uint8_t mask_bit = masked ? 0x80u : 0x00u;

    if (size < 126)
    {
        frame.push_back((uint8_t)(mask_bit | (uint8_t)size));
    }
    else if (size <= 0xFFFF)
    {
        frame.push_back((uint8_t)(mask_bit | 126u));
        frame.push_back((uint8_t)((size >> 8) & 0xFFu));
        frame.push_back((uint8_t)(size & 0xFFu));
    }
    else
    {
        frame.push_back((uint8_t)(mask_bit | 127u));

        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)(((uint64_t)size >> (i * 8)) & 0xFFu));
    }

    if (masked)
    {
        for (int i = 0; i < 4; i++)
            frame.push_back(mask[i]);

        for (size_t i = 0; i < size; i++)
            frame.push_back((uint8_t)(payload[i] ^ mask[i % 4]));
    }
    else
    {
        for (size_t i = 0; i < size; i++)
            frame.push_back(payload[i]);
    }

    return frame;
}

static std::vector<uint8_t> MakeMaskedFrame(uint8_t opcode, const std::vector<uint8_t>& payload)
{
    static const uint8_t k_mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
    return MakeClientFrame(opcode, payload, true, k_mask, true);
}

static const char* k_rfc_request =
    "GET /chat HTTP/1.1\r\n"
    "Host: server.example.com\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Protocol: chat, superchat\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

static void TestHandshakeResponse()
{
    std::string key;
    Check(liveview_ws_parse_handshake(k_rfc_request, strlen(k_rfc_request), key) ==
        LIVEVIEW_WS_HANDSHAKE_OK, "the RFC 6455 example request is accepted");
    CheckEqual(key, "dGhlIHNhbXBsZSBub25jZQ==", "the client key is extracted verbatim");

    std::string response;
    Check(liveview_ws_handshake_response(k_rfc_request, strlen(k_rfc_request), response) ==
        LIVEVIEW_WS_HANDSHAKE_OK, "the RFC 6455 example request yields a response");
    CheckEqual(response,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
        "\r\n",
        "the 101 response matches the RFC 6455 example");

    // Header names are case insensitive and Connection carries a token list.
    const char* mixed_case_request =
        "GET /ws HTTP/1.1\r\n"
        "host: 127.0.0.1:7778\r\n"
        "UPGRADE: WebSocket\r\n"
        "connection: keep-alive, Upgrade\r\n"
        "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    std::string mixed_response;
    Check(liveview_ws_handshake_response(mixed_case_request, strlen(mixed_case_request),
        mixed_response) == LIVEVIEW_WS_HANDSHAKE_OK,
        "header names and tokens are matched case insensitively");
    Check(mixed_response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") !=
        std::string::npos, "the accept value does not depend on header casing");

    std::string accept;
    Check(liveview_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept),
        "the accept value is computed for a non empty key");
    CheckEqual(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "the accept value matches RFC 6455");
    Check(!liveview_ws_accept_key("", accept), "an empty key has no accept value");
}

static void TestHandshakeErrors()
{
    const char* without_key =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:7778\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";
    std::string response = "untouched";
    Check(liveview_ws_handshake_response(without_key, strlen(without_key), response) ==
        LIVEVIEW_WS_HANDSHAKE_MISSING_KEY, "a request without a key is rejected");
    CheckEqual(response, "untouched", "a rejected handshake writes no response");

    const char* empty_key =
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key:   \r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(empty_key, strlen(empty_key), response) ==
        LIVEVIEW_WS_HANDSHAKE_MISSING_KEY, "a request with an empty key is rejected");

    const char* without_upgrade =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:7778\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(without_upgrade, strlen(without_upgrade), response) ==
        LIVEVIEW_WS_HANDSHAKE_MISSING_UPGRADE,
        "a request without the upgrade header is rejected");

    const char* without_connection =
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(without_connection, strlen(without_connection),
        response) == LIVEVIEW_WS_HANDSHAKE_MISSING_UPGRADE,
        "a request without the connection header is rejected");

    const char* wrong_upgrade_value =
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: h2c\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(wrong_upgrade_value, strlen(wrong_upgrade_value),
        response) == LIVEVIEW_WS_HANDSHAKE_MISSING_UPGRADE,
        "an upgrade to another protocol is rejected");

    std::string partial(k_rfc_request);
    partial = partial.substr(0, partial.size() - 4);
    Check(liveview_ws_handshake_response(partial.data(), partial.size(), response) ==
        LIVEVIEW_WS_HANDSHAKE_INCOMPLETE,
        "a header block without a terminating empty line is incomplete");
    Check(liveview_ws_handshake_response(NULL, 0, response) ==
        LIVEVIEW_WS_HANDSHAKE_INCOMPLETE, "an empty request is incomplete");

    const char* post_request =
        "POST /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(post_request, strlen(post_request), response) ==
        LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST, "a non GET request is rejected");

    const char* broken_header =
        "GET /ws HTTP/1.1\r\n"
        "this line has no colon\r\n"
        "\r\n";
    Check(liveview_ws_handshake_response(broken_header, strlen(broken_header), response) ==
        LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST, "a malformed header line is rejected");
}

static void TestServerFrameLengthForms()
{
    std::vector<uint8_t> frame;

    std::vector<uint8_t> small = MakePayload(125);
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_BINARY, &small[0], small.size(), frame);
    Check(frame.size() == 2 + 125, "a 125 byte payload uses a two byte header");
    Check(frame[0] == 0x82, "the frame is final and binary");
    Check(frame[1] == 125, "the 7 bit length form carries the length directly");
    Check(memcmp(&frame[2], &small[0], small.size()) == 0, "the payload is copied unchanged");

    std::vector<uint8_t> medium = MakePayload(126);
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_TEXT, &medium[0], medium.size(), frame);
    Check(frame.size() == 4 + 126, "a 126 byte payload uses a four byte header");
    Check(frame[0] == 0x81, "the frame is final and text");
    Check(frame[1] == 126, "the 16 bit length form is selected at 126 bytes");
    Check(frame[2] == 0x00 && frame[3] == 126, "the 16 bit length is big endian");

    std::vector<uint8_t> large = MakePayload(65535);
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_BINARY, &large[0], large.size(), frame);
    Check(frame.size() == 4 + 65535, "a 65535 byte payload still uses the 16 bit form");
    Check(frame[1] == 126, "65535 bytes stay in the 16 bit length form");
    Check(frame[2] == 0xFF && frame[3] == 0xFF, "the 16 bit length uses its full range");

    std::vector<uint8_t> huge = MakePayload(65536);
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_BINARY, &huge[0], huge.size(), frame);
    Check(frame.size() == 10 + 65536, "a 65536 byte payload uses a ten byte header");
    Check(frame[1] == 127, "the 64 bit length form is selected at 65536 bytes");
    Check(frame[2] == 0 && frame[3] == 0 && frame[4] == 0 && frame[5] == 0,
        "the upper 64 bit length bytes are zero");
    Check(frame[6] == 0x00 && frame[7] == 0x01 && frame[8] == 0x00 && frame[9] == 0x00,
        "the 64 bit length is big endian");

    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_TEXT, NULL, 0, frame);
    Check(frame.size() == 2, "an empty payload produces a bare header");
    Check(frame[1] == 0, "an empty payload has length zero");

    // Server frames are never masked.
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_PING, &small[0], 4, frame);
    Check((frame[1] & 0x80) == 0, "server frames carry no mask bit");
    Check(frame[0] == 0x89, "ping frames use opcode 0x9");

    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_PONG, NULL, 0, frame);
    Check(frame[0] == 0x8A, "pong frames use opcode 0xA");
}

// Reads the payload of a server frame back, so encode and decode can be paired
// without a second implementation of the length forms in the test.
static std::vector<uint8_t> ServerFramePayload(const std::vector<uint8_t>& frame)
{
    size_t header = 2;
    uint64_t length = frame[1] & 0x7F;

    if (length == 126)
    {
        length = ((uint64_t)frame[2] << 8) | (uint64_t)frame[3];
        header = 4;
    }
    else if (length == 127)
    {
        length = 0;

        for (int i = 0; i < 8; i++)
            length = (length << 8) | (uint64_t)frame[2 + i];

        header = 10;
    }

    Check(frame.size() == header + (size_t)length, "the server frame length matches its header");

    return std::vector<uint8_t>(frame.begin() + (std::ptrdiff_t)header, frame.end());
}

static void TestRoundtrip()
{
    static const size_t k_sizes[] = { 0, 1, 124, 125, 126, 127, 1000, 65534, 65535, 65536, 65537 };
    static const uint8_t k_opcodes[] = { LIVEVIEW_WS_OPCODE_TEXT, LIVEVIEW_WS_OPCODE_BINARY };

    for (size_t o = 0; o < sizeof(k_opcodes) / sizeof(k_opcodes[0]); o++)
    {
        for (size_t s = 0; s < sizeof(k_sizes) / sizeof(k_sizes[0]); s++)
        {
            std::vector<uint8_t> payload = MakePayload(k_sizes[s]);
            std::vector<uint8_t> encoded;
            liveview_ws_encode_frame((LiveviewWsOpcode)k_opcodes[o],
                payload.empty() ? NULL : &payload[0], payload.size(), encoded);

            Check(ServerFramePayload(encoded) == payload,
                "the encoded server frame carries the original payload");

            // The same payload, sent back by a client, decodes to the original.
            std::vector<uint8_t> client_frame = MakeMaskedFrame(k_opcodes[o], payload);
            LiveviewWsDecoder decoder;
            decoder.Push(&client_frame[0], client_frame.size());

            LiveviewWsFrame frame;
            Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME,
                "a complete client frame decodes in one step");
            Check(frame.opcode == k_opcodes[o], "the opcode survives the roundtrip");
            Check(frame.fin, "the decoded frame is final");
            Check(frame.payload == payload, "the demasked payload matches the original");
            Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_NEED_MORE,
                "no further frame is reported after the buffer is drained");
        }
    }
}

static void TestIncrementalDecoding()
{
    std::vector<uint8_t> payload = MakePayload(70000);
    std::vector<uint8_t> client_frame = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_BINARY, payload);

    // Reference: the frame decoded in one piece.
    LiveviewWsDecoder whole;
    whole.Push(&client_frame[0], client_frame.size());
    LiveviewWsFrame reference;
    Check(whole.Next(reference) == LIVEVIEW_WS_DECODE_FRAME, "the reference frame decodes");

    static const size_t k_chunk_sizes[] = { 1, 2, 3, 5, 7, 9, 64, 1024, 65536 };

    for (size_t c = 0; c < sizeof(k_chunk_sizes) / sizeof(k_chunk_sizes[0]); c++)
    {
        LiveviewWsDecoder decoder;
        LiveviewWsFrame frame;
        size_t offset = 0;
        bool decoded = false;

        while (offset < client_frame.size())
        {
            size_t chunk = k_chunk_sizes[c];

            if (chunk > client_frame.size() - offset)
                chunk = client_frame.size() - offset;

            decoder.Push(&client_frame[offset], chunk);
            offset += chunk;

            LiveviewWsDecodeStatus status = decoder.Next(frame);
            Check(status != LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
                "a partially received frame is not a protocol error");

            if (status == LIVEVIEW_WS_DECODE_FRAME)
            {
                Check(offset == client_frame.size(),
                    "the frame is only reported once all of its bytes arrived");
                decoded = true;
            }
        }

        Check(decoded, "the chunked frame is eventually reported");
        Check(frame.payload == reference.payload,
            "chunked decoding yields the same payload as decoding at once");
        Check(frame.opcode == reference.opcode, "chunked decoding yields the same opcode");
    }

    // Pseudo random splits, deterministic seed.
    uint32_t seed = 0xC0FFEEu;

    for (int round = 0; round < 20; round++)
    {
        LiveviewWsDecoder decoder;
        LiveviewWsFrame frame;
        size_t offset = 0;
        bool decoded = false;

        while (offset < client_frame.size())
        {
            seed = (seed * 1103515245u) + 12345u;
            size_t chunk = (size_t)((seed >> 16) % 4096u) + 1u;

            if (chunk > client_frame.size() - offset)
                chunk = client_frame.size() - offset;

            decoder.Push(&client_frame[offset], chunk);
            offset += chunk;

            if (decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME)
                decoded = true;
        }

        Check(decoded, "the randomly split frame is eventually reported");
        Check(frame.payload == reference.payload,
            "randomly split decoding yields the same payload");
    }

    // Several frames arriving in one read are reported one by one.
    LiveviewWsDecoder batched;
    std::vector<uint8_t> first = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_TEXT, MakePayload(10));
    std::vector<uint8_t> second = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_BINARY, MakePayload(200));
    std::vector<uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());
    batched.Push(&stream[0], stream.size());

    LiveviewWsFrame frame;
    Check(batched.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "the first batched frame decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_TEXT && frame.payload == MakePayload(10),
        "the first batched frame keeps its payload");
    Check(batched.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "the second batched frame decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_BINARY && frame.payload == MakePayload(200),
        "the second batched frame keeps its payload");
    Check(batched.Next(frame) == LIVEVIEW_WS_DECODE_NEED_MORE, "the stream is drained");
}

static void TestProtocolErrors()
{
    static const uint8_t k_mask[4] = { 0x01, 0x02, 0x03, 0x04 };

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> payload = MakePayload(8);
        std::vector<uint8_t> unmasked = MakeClientFrame(LIVEVIEW_WS_OPCODE_TEXT, payload, true,
            k_mask, false);
        decoder.Push(&unmasked[0], unmasked.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "an unmasked client frame is a protocol error");
        Check(strlen(decoder.GetError()) > 0, "the protocol error is named");
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "the decoder stays in the error state");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> reserved = MakeMaskedFrame(0x3, MakePayload(4));
        decoder.Push(&reserved[0], reserved.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a reserved data opcode is a protocol error");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> reserved = MakeMaskedFrame(0xB, MakePayload(4));
        decoder.Push(&reserved[0], reserved.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a reserved control opcode is a protocol error");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> rsv = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_TEXT, MakePayload(4));
        rsv[0] = (uint8_t)(rsv[0] | 0x40);
        decoder.Push(&rsv[0], rsv.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a set reserved bit is a protocol error");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> fragmented_control = MakeClientFrame(LIVEVIEW_WS_OPCODE_PING,
            MakePayload(4), false, k_mask, true);
        decoder.Push(&fragmented_control[0], fragmented_control.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a fragmented control frame is a protocol error");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> big_control = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_PING,
            MakePayload(126));
        decoder.Push(&big_control[0], big_control.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "an oversized control frame is a protocol error");
    }

    {
        // A 64 bit length with the high bit set is invalid per RFC 6455.
        LiveviewWsDecoder decoder;
        uint8_t header[14];
        memset(header, 0, sizeof(header));
        header[0] = 0x82;
        header[1] = (uint8_t)(0x80 | 127);
        header[2] = 0x80;
        decoder.Push(header, sizeof(header));

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a length with the high bit set is a protocol error");
    }

    {
        // Beyond the accepted maximum the frame is refused instead of allocated.
        LiveviewWsDecoder decoder;
        uint8_t header[14];
        memset(header, 0, sizeof(header));
        header[0] = 0x82;
        header[1] = (uint8_t)(0x80 | 127);
        uint64_t length = (uint64_t)LIVEVIEW_WS_MAX_PAYLOAD_SIZE + 1u;

        for (int i = 0; i < 8; i++)
            header[2 + i] = (uint8_t)((length >> ((7 - i) * 8)) & 0xFFu);

        decoder.Push(header, sizeof(header));

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "an oversized payload is a protocol error");
    }

    {
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> truncated_close = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_CLOSE,
            MakePayload(1));
        decoder.Push(&truncated_close[0], truncated_close.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "a close frame with a single payload byte is a protocol error");
    }

    {
        // After Reset the decoder accepts a valid stream again.
        LiveviewWsDecoder decoder;
        std::vector<uint8_t> unmasked = MakeClientFrame(LIVEVIEW_WS_OPCODE_TEXT, MakePayload(8),
            true, k_mask, false);
        decoder.Push(&unmasked[0], unmasked.size());

        LiveviewWsFrame frame;
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR,
            "the decoder reports the unmasked frame");
        decoder.Reset();
        Check(strlen(decoder.GetError()) == 0, "Reset clears the error state");

        std::vector<uint8_t> valid = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_TEXT, MakePayload(8));
        decoder.Push(&valid[0], valid.size());
        Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME,
            "a reset decoder decodes the next stream");
    }
}

static void TestControlFrames()
{
    std::vector<uint8_t> encoded;
    liveview_ws_encode_close(1000, "bye", encoded);
    Check(encoded[0] == 0x88, "close frames use opcode 0x8");
    Check(encoded[1] == 5, "the close payload holds the code and the reason");
    Check(encoded[2] == 0x03 && encoded[3] == 0xE8, "the close code is big endian");
    Check(memcmp(&encoded[4], "bye", 3) == 0, "the close reason is copied");

    liveview_ws_encode_close(1001, NULL, encoded);
    Check(encoded[1] == 2, "a close frame without a reason carries only the code");

    // A client close frame decodes into code and reason.
    std::vector<uint8_t> payload;
    payload.push_back(0x03);
    payload.push_back(0xE9);
    payload.push_back('o');
    payload.push_back('k');

    LiveviewWsDecoder decoder;
    std::vector<uint8_t> client_close = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_CLOSE, payload);
    decoder.Push(&client_close[0], client_close.size());

    LiveviewWsFrame frame;
    Check(decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "a client close frame decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_CLOSE, "the close opcode is reported");
    Check(frame.close_code == 1001, "the close code is extracted");
    Check(frame.payload.size() == 2 && frame.payload[0] == 'o' && frame.payload[1] == 'k',
        "the close reason is reported without the code bytes");

    // An empty close frame reports the absent code.
    LiveviewWsDecoder empty_decoder;
    std::vector<uint8_t> empty_close = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_CLOSE,
        std::vector<uint8_t>());
    empty_decoder.Push(&empty_close[0], empty_close.size());
    Check(empty_decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "an empty close frame decodes");
    Check(frame.close_code == LIVEVIEW_WS_CLOSE_CODE_ABSENT,
        "a close frame without a code reports the absent code");

    // Ping and pong roundtrip through the decoder.
    LiveviewWsDecoder ping_decoder;
    std::vector<uint8_t> ping_payload = MakePayload(20);
    std::vector<uint8_t> client_ping = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_PING, ping_payload);
    ping_decoder.Push(&client_ping[0], client_ping.size());
    Check(ping_decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "a client ping decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_PING, "the ping opcode is reported");
    Check(frame.payload == ping_payload, "the ping payload is demasked");

    LiveviewWsDecoder pong_decoder;
    std::vector<uint8_t> client_pong = MakeMaskedFrame(LIVEVIEW_WS_OPCODE_PONG,
        std::vector<uint8_t>());
    pong_decoder.Push(&client_pong[0], client_pong.size());
    Check(pong_decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "an empty client pong decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_PONG, "the pong opcode is reported");
    Check(frame.payload.empty(), "the empty pong has no payload");

    // Fragmented data frames are reported one by one with their fin flag.
    static const uint8_t k_mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    LiveviewWsDecoder fragment_decoder;
    std::vector<uint8_t> head = MakeClientFrame(LIVEVIEW_WS_OPCODE_TEXT, MakePayload(4), false,
        k_mask, true);
    std::vector<uint8_t> tail = MakeClientFrame(LIVEVIEW_WS_OPCODE_CONTINUATION, MakePayload(6),
        true, k_mask, true);
    fragment_decoder.Push(&head[0], head.size());
    fragment_decoder.Push(&tail[0], tail.size());

    Check(fragment_decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "the first fragment decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_TEXT && !frame.fin,
        "the first fragment is not final");
    Check(fragment_decoder.Next(frame) == LIVEVIEW_WS_DECODE_FRAME, "the last fragment decodes");
    Check(frame.opcode == LIVEVIEW_WS_OPCODE_CONTINUATION && frame.fin,
        "the last fragment continues the message and is final");
}

int main()
{
    TestHandshakeResponse();
    TestHandshakeErrors();
    TestServerFrameLengthForms();
    TestRoundtrip();
    TestIncrementalDecoding();
    TestProtocolErrors();
    TestControlFrames();

    printf("All live view websocket tests passed\n");

    return 0;
}
