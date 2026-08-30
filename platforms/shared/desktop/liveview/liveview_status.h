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

#ifndef LIVEVIEW_STATUS_H
#define LIVEVIEW_STATUS_H

#include <stdint.h>
#include <chrono>
#include <mutex>
#include <string>

// The agent status posted through the MCP tool set_agent_status. It lives next
// to the live view because both the MCP side, which writes it, and the emulator
// main loop, which serializes it into the status channel, need it while neither
// owns the other. The store exists whether or not a live view server runs, so
// the tool stays valid without one.
//
// Header only on purpose: no new translation unit, and the inline accessor
// keeps one store per process.

// The longest agent text kept, in bytes (spec D4).
#define LIVEVIEW_AGENT_TEXT_MAX_BYTES 4096

struct LiveviewAgentStatus
{
    LiveviewAgentStatus()
    {
        timestamp_ms = 0;
        seq = 0;
    }

    std::string text;
    uint64_t timestamp_ms;
    // Counts the agent status updates. Never decreases (spec D3), grows by one
    // with every accepted set_agent_status call (spec D4).
    uint64_t seq;
};

struct LiveviewStatusStore
{
    std::mutex mutex;
    LiveviewAgentStatus agent;
};

inline LiveviewStatusStore& liveview_status_store(void)
{
    static LiveviewStatusStore store;
    return store;
}

inline uint64_t liveview_status_now_ms(void)
{
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    return (uint64_t)now.count();
}

// Cuts the text to at most LIVEVIEW_AGENT_TEXT_MAX_BYTES bytes without leaving
// half of a utf-8 sequence behind.
inline std::string liveview_status_clamp_text(const std::string& text)
{
    if (text.size() <= LIVEVIEW_AGENT_TEXT_MAX_BYTES)
        return text;

    size_t size = LIVEVIEW_AGENT_TEXT_MAX_BYTES;

    while ((size > 0) && (((unsigned char)text[size] & 0xC0) == 0x80))
        size--;

    return text.substr(0, size);
}

// Replaces the agent text, stamps it and counts the update. Returns the text
// as it was stored.
inline std::string liveview_status_set_agent_text(const std::string& text)
{
    std::string stored = liveview_status_clamp_text(text);
    LiveviewStatusStore& store = liveview_status_store();

    std::lock_guard<std::mutex> lock(store.mutex);
    store.agent.text = stored;
    store.agent.timestamp_ms = liveview_status_now_ms();
    store.agent.seq++;

    return stored;
}

inline LiveviewAgentStatus liveview_status_get_agent(void)
{
    LiveviewStatusStore& store = liveview_status_store();

    std::lock_guard<std::mutex> lock(store.mutex);
    return store.agent;
}

// Returns the text as a json string value without the surrounding quotes.
// Control characters become escapes, a byte sequence that is not valid utf-8
// becomes a question mark, so that the result is always a parseable json
// string.
inline std::string liveview_status_escape_json(const std::string& text)
{
    static const char* const k_hex_digits = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(text.size());

    size_t i = 0;

    while (i < text.size())
    {
        unsigned char character = (unsigned char)text[i];

        if (character < 0x80)
        {
            switch (character)
            {
                case '"':
                    escaped += "\\\"";
                    break;
                case '\\':
                    escaped += "\\\\";
                    break;
                case '\b':
                    escaped += "\\b";
                    break;
                case '\f':
                    escaped += "\\f";
                    break;
                case '\n':
                    escaped += "\\n";
                    break;
                case '\r':
                    escaped += "\\r";
                    break;
                case '\t':
                    escaped += "\\t";
                    break;
                default:
                    if ((character < 0x20) || (character == 0x7F))
                    {
                        escaped += "\\u00";
                        escaped += k_hex_digits[(character >> 4) & 0x0F];
                        escaped += k_hex_digits[character & 0x0F];
                    }
                    else
                        escaped += (char)character;
                    break;
            }

            i++;
            continue;
        }

        size_t length = 0;

        if ((character & 0xE0) == 0xC0)
            length = 2;
        else if ((character & 0xF0) == 0xE0)
            length = 3;
        else if ((character & 0xF8) == 0xF0)
            length = 4;

        bool valid = (length > 0) && ((i + length) <= text.size());

        for (size_t offset = 1; valid && (offset < length); offset++)
        {
            if (((unsigned char)text[i + offset] & 0xC0) != 0x80)
                valid = false;
        }

        if (!valid)
        {
            escaped += '?';
            i++;
            continue;
        }

        escaped.append(text, i, length);
        i += length;
    }

    return escaped;
}

#endif /* LIVEVIEW_STATUS_H */
