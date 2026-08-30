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

#ifndef LIVEVIEW_SERVER_H
#define LIVEVIEW_SERVER_H

#include <stdint.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Frame broadcast pacing: one PNG every 50 ms, about 20 fps.
#define LIVEVIEW_FRAME_INTERVAL_MS 50
// Status broadcast pacing: at most 4 messages per second.
#define LIVEVIEW_STATUS_INTERVAL_MS 250
// Poll timeout of the server thread, short enough to keep the pacing steady.
#define LIVEVIEW_POLL_TIMEOUT_MS 5
// A request header block larger than this is rejected.
#define LIVEVIEW_MAX_REQUEST_SIZE (16 * 1024)
// A client whose send queue grows past this is dropped.
#define LIVEVIEW_MAX_SEND_QUEUE_SIZE (4 * 1024 * 1024)

#ifdef _WIN32
typedef uintptr_t LiveviewSocket;
#else
typedef int LiveviewSocket;
#endif

struct LiveviewClient;

// The live view server: an HTTP and WebSocket listener in its own thread.
//
// The thread never reads emulator state. Everything it publishes comes from the
// two snapshots below, both filled by the emulator main loop through
// PublishFrame and PublishStatus and both guarded by a mutex that is only ever
// held for a copy.
class LiveviewServer
{
public:
    LiveviewServer();
    ~LiveviewServer();

    // Sets the page body served on GET /. May be called at any time.
    void SetPage(const std::string& html);

    // Binds and listens, then runs the server thread. Returns false when the
    // server is already running or the socket cannot be bound. A port of 0
    // binds an ephemeral port, readable through GetPort.
    bool Start(const char* address, int port);

    // Stops the server thread and closes every connection. Returns once the
    // thread is joined. Safe to call on a server that is not running.
    void Stop();

    bool IsRunning() const;

    // The bound port, 0 while the server is not running.
    int GetPort() const;

    // Copies an image snapshot for the server thread to encode and broadcast.
    // Non blocking apart from the copy under the snapshot mutex.
    void PublishFrame(const void* pixels, int width, int height, int channels);

    // Copies a status string for the server thread to broadcast on change.
    void PublishStatus(const std::string& status);

private:
    void ThreadFunc(void);
    void AcceptClient(void);
    // Returns false when the client has to be dropped.
    bool ReadClient(LiveviewClient* client);
    bool HandleRequest(LiveviewClient* client);
    bool HandleWebsocketBytes(LiveviewClient* client, const uint8_t* data, size_t size);
    void QueueHttpResponse(LiveviewClient* client, const char* status_line,
        const char* content_type, const std::string& body);
    void QueueBytes(LiveviewClient* client, const void* data, size_t size);
    // Returns false when the client has to be dropped.
    bool FlushClient(LiveviewClient* client);
    void DropClient(size_t index);
    void CloseAllClients(void);
    void BroadcastFrame(void);
    void BroadcastStatus(void);
    void BroadcastToClients(const std::vector<uint8_t>& frame, bool droppable);

private:
    std::atomic<bool> m_running;
    std::atomic<int> m_port;
    std::thread m_thread;
    LiveviewSocket m_listen_socket;
    std::vector<LiveviewClient*> m_clients;

    mutable std::mutex m_page_mutex;
    std::string m_page;

    std::mutex m_snapshot_mutex;
    std::vector<uint8_t> m_frame_pixels;
    int m_frame_width;
    int m_frame_height;
    int m_frame_channels;
    uint64_t m_frame_seq;
    std::string m_status;
    uint64_t m_status_seq;

    // Server thread only.
    std::vector<uint8_t> m_encode_pixels;
    uint64_t m_last_frame_seq;
    uint64_t m_last_status_seq;
};

#endif /* LIVEVIEW_SERVER_H */
