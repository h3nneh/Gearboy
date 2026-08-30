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

#ifndef LIVEVIEW_WS_H
#define LIVEVIEW_WS_H

#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

// Socket free WebSocket protocol helpers for the live view server: handshake
// handling and the server side frame codec. Nothing here talks to a socket, so
// every function can be exercised by unit tests.

#define LIVEVIEW_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define LIVEVIEW_WS_MAX_PAYLOAD_SIZE (4 * 1024 * 1024)
#define LIVEVIEW_WS_CLOSE_CODE_ABSENT 1005

enum LiveviewWsOpcode
{
    LIVEVIEW_WS_OPCODE_CONTINUATION = 0x0,
    LIVEVIEW_WS_OPCODE_TEXT = 0x1,
    LIVEVIEW_WS_OPCODE_BINARY = 0x2,
    LIVEVIEW_WS_OPCODE_CLOSE = 0x8,
    LIVEVIEW_WS_OPCODE_PING = 0x9,
    LIVEVIEW_WS_OPCODE_PONG = 0xA
};

enum LiveviewWsHandshakeStatus
{
    LIVEVIEW_WS_HANDSHAKE_OK = 0,
    // The header block is not terminated by an empty line yet, more bytes of
    // the request are needed.
    LIVEVIEW_WS_HANDSHAKE_INCOMPLETE,
    // The request line is not a well formed GET request.
    LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST,
    // Upgrade: websocket and Connection: Upgrade are not both present.
    LIVEVIEW_WS_HANDSHAKE_MISSING_UPGRADE,
    // Sec-WebSocket-Key is missing or empty.
    LIVEVIEW_WS_HANDSHAKE_MISSING_KEY
};

enum LiveviewWsDecodeStatus
{
    // No complete frame in the buffer yet, feed more bytes.
    LIVEVIEW_WS_DECODE_NEED_MORE = 0,
    // A complete frame was written to the output frame.
    LIVEVIEW_WS_DECODE_FRAME,
    // The stream violates the protocol, the connection has to be dropped.
    LIVEVIEW_WS_DECODE_PROTOCOL_ERROR
};

struct LiveviewWsFrame
{
    LiveviewWsFrame();

    void Clear();

    uint8_t opcode;
    bool fin;
    std::vector<uint8_t> payload;
    // For close frames: the status code, or LIVEVIEW_WS_CLOSE_CODE_ABSENT when
    // the frame carries no code. The payload of a close frame holds the reason
    // only, without the two code bytes.
    uint16_t close_code;
};

// Extracts the Sec-WebSocket-Key from an HTTP upgrade request header block.
// key is only written on LIVEVIEW_WS_HANDSHAKE_OK.
LiveviewWsHandshakeStatus liveview_ws_parse_handshake(const char* request, size_t size,
    std::string& key);

// Computes the Sec-WebSocket-Accept value for a client key. Returns false for
// an empty key.
bool liveview_ws_accept_key(const std::string& key, std::string& accept);

// Builds the complete HTTP 101 response, terminating empty line included, for
// an HTTP upgrade request header block. response is only written on
// LIVEVIEW_WS_HANDSHAKE_OK.
LiveviewWsHandshakeStatus liveview_ws_handshake_response(const char* request, size_t size,
    std::string& response);

// Encodes an unmasked server frame, replacing the previous content of out. The
// frame is always final (FIN set); payload may be null when size is zero.
void liveview_ws_encode_frame(LiveviewWsOpcode opcode, const void* payload, size_t size,
    std::vector<uint8_t>& out);

// Encodes an unmasked close frame carrying a status code and an optional
// reason, replacing the previous content of out.
void liveview_ws_encode_close(uint16_t code, const char* reason, std::vector<uint8_t>& out);

// Incremental decoder for masked client frames. Bytes arrive in arbitrary
// chunks through Push, complete frames leave through Next. Fragmented messages
// are reported frame by frame; reassembling them is up to the caller.
class LiveviewWsDecoder
{
public:
    LiveviewWsDecoder();

    void Push(const void* data, size_t size);
    LiveviewWsDecodeStatus Next(LiveviewWsFrame& frame);

    // Describes the last protocol error, empty while none was reported.
    const char* GetError() const;

    void Reset();

    size_t GetPendingBytes() const;

private:
    LiveviewWsDecodeStatus Failed(const char* error);

private:
    std::vector<uint8_t> m_buffer;
    size_t m_offset;
    const char* m_error;
};

#endif /* LIVEVIEW_WS_H */
