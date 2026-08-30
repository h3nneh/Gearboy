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

#include "liveview_ws.h"

#include <cstring>
#include "liveview_crypto.h"

#define LIVEVIEW_WS_COMPACT_THRESHOLD (64 * 1024)

static char LiveviewWsLower(char value)
{
    if (value >= 'A' && value <= 'Z')
        return (char)(value - 'A' + 'a');

    return value;
}

static std::string LiveviewWsToLower(const std::string& text)
{
    std::string lowered = text;

    for (size_t i = 0; i < lowered.size(); i++)
        lowered[i] = LiveviewWsLower(lowered[i]);

    return lowered;
}

static std::string LiveviewWsTrim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();

    while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        begin++;

    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
        text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        end--;
    }

    return text.substr(begin, end - begin);
}

// True when value carries token as one of its comma separated entries.
static bool LiveviewWsHasToken(const std::string& value, const char* token)
{
    std::string lowered = LiveviewWsToLower(value);
    std::string wanted = token;
    size_t begin = 0;

    while (begin <= lowered.size())
    {
        size_t separator = lowered.find(',', begin);
        size_t end = (separator == std::string::npos) ? lowered.size() : separator;
        std::string entry = LiveviewWsTrim(lowered.substr(begin, end - begin));

        if (entry == wanted)
            return true;

        if (separator == std::string::npos)
            break;

        begin = separator + 1;
    }

    return false;
}

static bool LiveviewWsFindHeaderEnd(const std::string& request, size_t& header_end)
{
    size_t position = request.find("\r\n\r\n");

    if (position != std::string::npos)
    {
        header_end = position;
        return true;
    }

    position = request.find("\n\n");

    if (position != std::string::npos)
    {
        header_end = position;
        return true;
    }

    return false;
}

LiveviewWsHandshakeStatus liveview_ws_parse_handshake(const char* request, size_t size,
    std::string& key)
{
    if (request == NULL || size == 0)
        return LIVEVIEW_WS_HANDSHAKE_INCOMPLETE;

    std::string text(request, size);
    size_t header_end = 0;

    if (!LiveviewWsFindHeaderEnd(text, header_end))
        return LIVEVIEW_WS_HANDSHAKE_INCOMPLETE;

    std::string headers = text.substr(0, header_end);
    size_t line_begin = 0;
    bool first_line = true;
    bool has_upgrade = false;
    bool has_connection = false;
    std::string found_key;

    while (line_begin <= headers.size())
    {
        size_t line_end = headers.find('\n', line_begin);
        size_t next_begin = (line_end == std::string::npos) ?
            headers.size() + 1 : line_end + 1;
        size_t take = ((line_end == std::string::npos) ? headers.size() : line_end) - line_begin;
        std::string line = LiveviewWsTrim(headers.substr(line_begin, take));
        line_begin = next_begin;

        if (first_line)
        {
            first_line = false;

            if (line.size() < 4 || line.compare(0, 4, "GET ") != 0)
                return LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST;

            if (line.find("HTTP/1.") == std::string::npos)
                return LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST;

            continue;
        }

        if (line.empty())
            continue;

        size_t colon = line.find(':');

        if (colon == std::string::npos)
            return LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST;

        std::string name = LiveviewWsToLower(LiveviewWsTrim(line.substr(0, colon)));
        std::string value = LiveviewWsTrim(line.substr(colon + 1));

        if (name == "upgrade")
        {
            if (LiveviewWsHasToken(value, "websocket"))
                has_upgrade = true;
        }
        else if (name == "connection")
        {
            if (LiveviewWsHasToken(value, "upgrade"))
                has_connection = true;
        }
        else if (name == "sec-websocket-key")
        {
            found_key = value;
        }
    }

    if (first_line)
        return LIVEVIEW_WS_HANDSHAKE_BAD_REQUEST;

    if (!has_upgrade || !has_connection)
        return LIVEVIEW_WS_HANDSHAKE_MISSING_UPGRADE;

    if (found_key.empty())
        return LIVEVIEW_WS_HANDSHAKE_MISSING_KEY;

    key = found_key;

    return LIVEVIEW_WS_HANDSHAKE_OK;
}

bool liveview_ws_accept_key(const std::string& key, std::string& accept)
{
    if (key.empty())
        return false;

    std::string combined = key + LIVEVIEW_WS_GUID;
    uint8_t digest[LIVEVIEW_SHA1_DIGEST_SIZE];
    liveview_sha1(combined.data(), combined.size(), digest);

    char encoded[64];

    if (!liveview_base64_encode(digest, sizeof(digest), encoded, sizeof(encoded)))
        return false;

    accept = encoded;

    return true;
}

LiveviewWsHandshakeStatus liveview_ws_handshake_response(const char* request, size_t size,
    std::string& response)
{
    std::string key;
    LiveviewWsHandshakeStatus status = liveview_ws_parse_handshake(request, size, key);

    if (status != LIVEVIEW_WS_HANDSHAKE_OK)
        return status;

    std::string accept;

    if (!liveview_ws_accept_key(key, accept))
        return LIVEVIEW_WS_HANDSHAKE_MISSING_KEY;

    response = "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: " + accept + "\r\n";
    response += "\r\n";

    return LIVEVIEW_WS_HANDSHAKE_OK;
}

void liveview_ws_encode_frame(LiveviewWsOpcode opcode, const void* payload, size_t size,
    std::vector<uint8_t>& out)
{
    out.clear();
    out.push_back((uint8_t)(0x80u | ((uint8_t)opcode & 0x0Fu)));

    if (size < 126u)
    {
        out.push_back((uint8_t)size);
    }
    else if (size <= 0xFFFFu)
    {
        out.push_back(126u);
        out.push_back((uint8_t)((size >> 8) & 0xFFu));
        out.push_back((uint8_t)(size & 0xFFu));
    }
    else
    {
        uint64_t length = (uint64_t)size;
        out.push_back(127u);

        for (int i = 7; i >= 0; i--)
            out.push_back((uint8_t)((length >> (i * 8)) & 0xFFu));
    }

    if (size > 0 && payload != NULL)
    {
        const uint8_t* bytes = (const uint8_t*)payload;
        out.insert(out.end(), bytes, bytes + size);
    }
}

void liveview_ws_encode_close(uint16_t code, const char* reason, std::vector<uint8_t>& out)
{
    std::vector<uint8_t> payload;
    payload.push_back((uint8_t)((code >> 8) & 0xFFu));
    payload.push_back((uint8_t)(code & 0xFFu));

    if (reason != NULL)
    {
        size_t length = strlen(reason);

        if (length > 123u)
            length = 123u;

        const uint8_t* bytes = (const uint8_t*)reason;
        payload.insert(payload.end(), bytes, bytes + length);
    }

    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_CLOSE, payload.empty() ? NULL : &payload[0],
        payload.size(), out);
}

LiveviewWsFrame::LiveviewWsFrame()
{
    Clear();
}

void LiveviewWsFrame::Clear()
{
    opcode = LIVEVIEW_WS_OPCODE_CONTINUATION;
    fin = false;
    payload.clear();
    close_code = LIVEVIEW_WS_CLOSE_CODE_ABSENT;
}

LiveviewWsDecoder::LiveviewWsDecoder()
{
    m_offset = 0;
    m_error = "";
}

void LiveviewWsDecoder::Push(const void* data, size_t size)
{
    if (data == NULL || size == 0)
        return;

    const uint8_t* bytes = (const uint8_t*)data;
    m_buffer.insert(m_buffer.end(), bytes, bytes + size);
}

const char* LiveviewWsDecoder::GetError() const
{
    return m_error;
}

void LiveviewWsDecoder::Reset()
{
    m_buffer.clear();
    m_offset = 0;
    m_error = "";
}

size_t LiveviewWsDecoder::GetPendingBytes() const
{
    return m_buffer.size() - m_offset;
}

LiveviewWsDecodeStatus LiveviewWsDecoder::Failed(const char* error)
{
    m_error = error;

    return LIVEVIEW_WS_DECODE_PROTOCOL_ERROR;
}

static bool LiveviewWsIsKnownOpcode(uint8_t opcode)
{
    return (opcode == LIVEVIEW_WS_OPCODE_CONTINUATION) ||
           (opcode == LIVEVIEW_WS_OPCODE_TEXT) ||
           (opcode == LIVEVIEW_WS_OPCODE_BINARY) ||
           (opcode == LIVEVIEW_WS_OPCODE_CLOSE) ||
           (opcode == LIVEVIEW_WS_OPCODE_PING) ||
           (opcode == LIVEVIEW_WS_OPCODE_PONG);
}

LiveviewWsDecodeStatus LiveviewWsDecoder::Next(LiveviewWsFrame& frame)
{
    if (m_error[0] != '\0')
        return LIVEVIEW_WS_DECODE_PROTOCOL_ERROR;

    size_t available = m_buffer.size() - m_offset;

    if (available < 2u)
        return LIVEVIEW_WS_DECODE_NEED_MORE;

    const uint8_t* bytes = &m_buffer[m_offset];
    uint8_t first = bytes[0];
    uint8_t second = bytes[1];

    if ((first & 0x70u) != 0)
        return Failed("reserved frame bit set");

    uint8_t opcode = (uint8_t)(first & 0x0Fu);

    if (!LiveviewWsIsKnownOpcode(opcode))
        return Failed("reserved opcode");

    bool fin = (first & 0x80u) != 0;
    bool masked = (second & 0x80u) != 0;
    uint64_t length = (uint64_t)(second & 0x7Fu);
    size_t header = 2u;

    if (length == 126u)
    {
        if (available < 4u)
            return LIVEVIEW_WS_DECODE_NEED_MORE;

        length = ((uint64_t)bytes[2] << 8) | (uint64_t)bytes[3];
        header = 4u;
    }
    else if (length == 127u)
    {
        if (available < 10u)
            return LIVEVIEW_WS_DECODE_NEED_MORE;

        length = 0;

        for (int i = 0; i < 8; i++)
            length = (length << 8) | (uint64_t)bytes[2 + i];

        if ((length & 0x8000000000000000ULL) != 0)
            return Failed("payload length has the high bit set");

        header = 10u;
    }

    if (opcode >= 0x8u)
    {
        if (!fin)
            return Failed("fragmented control frame");

        if (length > 125u)
            return Failed("oversized control frame");
    }

    if (!masked)
        return Failed("client frame is not masked");

    if (length > (uint64_t)LIVEVIEW_WS_MAX_PAYLOAD_SIZE)
        return Failed("payload exceeds the accepted maximum size");

    size_t payload_size = (size_t)length;
    size_t total = header + 4u + payload_size;

    if (available < total)
        return LIVEVIEW_WS_DECODE_NEED_MORE;

    const uint8_t* mask = bytes + header;
    const uint8_t* data = mask + 4;

    frame.Clear();
    frame.opcode = opcode;
    frame.fin = fin;
    frame.payload.resize(payload_size);

    for (size_t i = 0; i < payload_size; i++)
        frame.payload[i] = (uint8_t)(data[i] ^ mask[i % 4u]);

    if (opcode == LIVEVIEW_WS_OPCODE_CLOSE)
    {
        if (payload_size == 1u)
            return Failed("close frame with a truncated status code");

        if (payload_size >= 2u)
        {
            frame.close_code = (uint16_t)(((uint16_t)frame.payload[0] << 8) |
                (uint16_t)frame.payload[1]);
            frame.payload.erase(frame.payload.begin(), frame.payload.begin() + 2);
        }
    }

    m_offset += total;

    if (m_offset == m_buffer.size())
    {
        m_buffer.clear();
        m_offset = 0;
    }
    else if (m_offset >= LIVEVIEW_WS_COMPACT_THRESHOLD)
    {
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + (std::ptrdiff_t)m_offset);
        m_offset = 0;
    }

    return LIVEVIEW_WS_DECODE_FRAME;
}
