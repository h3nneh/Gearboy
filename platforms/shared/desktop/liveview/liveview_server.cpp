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

#include "liveview_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include "liveview_ws.h"
#include "log.h"
#include "stb_image_write.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int liveview_socket_len_t;
    #define LIVEVIEW_INVALID_SOCKET INVALID_SOCKET
    #define LIVEVIEW_SOCKET_CLOSE(s) closesocket(s)
    #define LIVEVIEW_SOCKET_SHUTDOWN(s) shutdown(s, SD_BOTH)
    #define LIVEVIEW_SEND_FLAGS 0
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/select.h>
    #include <sys/time.h>
    #include <unistd.h>
    #include <fcntl.h>
    typedef socklen_t liveview_socket_len_t;
    #define LIVEVIEW_INVALID_SOCKET -1
    #define LIVEVIEW_SOCKET_CLOSE(s) ::close(s)
    #define LIVEVIEW_SOCKET_SHUTDOWN(s) ::shutdown(s, SHUT_RDWR)
    // A client that disappeared must not take the emulator down with SIGPIPE.
    #define LIVEVIEW_SEND_FLAGS MSG_NOSIGNAL
#endif

// One connected peer. Everything in here belongs to the server thread.
struct LiveviewClient
{
    LiveviewClient()
    {
        socket = LIVEVIEW_INVALID_SOCKET;
        websocket = false;
        close_after_flush = false;
        send_offset = 0;
    }

    LiveviewSocket socket;
    bool websocket;
    bool close_after_flush;
    std::string request;
    std::vector<uint8_t> send_queue;
    size_t send_offset;
    LiveviewWsDecoder decoder;
};

static const char* const k_liveview_placeholder_page =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><title>Gearboy Live View</title></head>\n"
    "<body><p>Gearboy live view placeholder page.</p></body>\n"
    "</html>\n";

static bool LiveviewWouldBlock(void)
{
#ifdef _WIN32
    int error = WSAGetLastError();
    return (error == WSAEWOULDBLOCK) || (error == WSAEINTR);
#else
    return (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR);
#endif
}

static bool LiveviewSetNonBlocking(LiveviewSocket socket)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Extracts the path of a GET request line. Returns false for anything else.
static bool LiveviewExtractGetPath(const std::string& line, std::string& path)
{
    if (line.size() < 5 || line.compare(0, 4, "GET ") != 0)
        return false;

    size_t path_end = line.find(' ', 4);

    if (path_end == std::string::npos || path_end == 4)
        return false;

    path = line.substr(4, path_end - 4);
    return true;
}

static void LiveviewPngWriter(void* context, void* data, int size)
{
    if (context == NULL || data == NULL || size <= 0)
        return;

    std::vector<uint8_t>* out = (std::vector<uint8_t>*)context;
    const uint8_t* bytes = (const uint8_t*)data;
    out->insert(out->end(), bytes, bytes + size);
}

LiveviewServer::LiveviewServer()
{
    m_running.store(false);
    m_port.store(0);
    m_listen_socket = LIVEVIEW_INVALID_SOCKET;
    m_frame_width = 0;
    m_frame_height = 0;
    m_frame_channels = 0;
    m_frame_seq = 0;
    m_status_seq = 0;
    m_last_frame_seq = 0;
    m_last_status_seq = 0;
}

LiveviewServer::~LiveviewServer()
{
    Stop();
}

void LiveviewServer::SetPage(const std::string& html)
{
    std::lock_guard<std::mutex> lock(m_page_mutex);
    m_page = html;
}

bool LiveviewServer::Start(const char* address, int port)
{
    if (m_running.load())
        return false;

    if (port < 0 || port > 65535)
    {
        Error("[LIVEVIEW] Invalid port: %d", port);
        return false;
    }

    std::string bind_address = ((address != NULL) && (address[0] != 0)) ? address : "127.0.0.1";

    if (bind_address == "localhost")
        bind_address = "127.0.0.1";

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    LiveviewSocket listen_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_socket == LIVEVIEW_INVALID_SOCKET)
    {
        Error("[LIVEVIEW] Failed to create socket");
        return false;
    }

    int option = 1;
#ifdef _WIN32
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&option, sizeof(option));
#else
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
#endif

    struct sockaddr_in socket_address;
    memset(&socket_address, 0, sizeof(socket_address));
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, bind_address.c_str(), &socket_address.sin_addr) != 1)
    {
        Error("[LIVEVIEW] Invalid bind address: %s", bind_address.c_str());
        LIVEVIEW_SOCKET_CLOSE(listen_socket);
        return false;
    }

    if (bind(listen_socket, (struct sockaddr*)&socket_address, sizeof(socket_address)) < 0)
    {
        Error("[LIVEVIEW] Failed to bind to %s:%d", bind_address.c_str(), port);
        LIVEVIEW_SOCKET_CLOSE(listen_socket);
        return false;
    }

    if (listen(listen_socket, 4) < 0)
    {
        Error("[LIVEVIEW] Failed to listen on %s:%d", bind_address.c_str(), port);
        LIVEVIEW_SOCKET_CLOSE(listen_socket);
        return false;
    }

    struct sockaddr_in bound_address;
    memset(&bound_address, 0, sizeof(bound_address));
    liveview_socket_len_t bound_length = sizeof(bound_address);
    int bound_port = port;

    if (getsockname(listen_socket, (struct sockaddr*)&bound_address, &bound_length) == 0)
        bound_port = (int)ntohs(bound_address.sin_port);

    if (!LiveviewSetNonBlocking(listen_socket))
    {
        Error("[LIVEVIEW] Failed to configure the listen socket");
        LIVEVIEW_SOCKET_CLOSE(listen_socket);
        return false;
    }

    m_listen_socket = listen_socket;
    m_port.store(bound_port);
    m_last_frame_seq = 0;
    m_last_status_seq = 0;
    m_running.store(true);
    m_thread = std::thread(&LiveviewServer::ThreadFunc, this);

    Log("[LIVEVIEW] Server listening on %s:%d", bind_address.c_str(), bound_port);

    return true;
}

void LiveviewServer::Stop(void)
{
    if (!m_running.exchange(false))
        return;

    if (m_thread.joinable())
        m_thread.join();

    if (m_listen_socket != LIVEVIEW_INVALID_SOCKET)
    {
        LIVEVIEW_SOCKET_CLOSE(m_listen_socket);
        m_listen_socket = LIVEVIEW_INVALID_SOCKET;
    }

    m_port.store(0);

#ifdef _WIN32
    WSACleanup();
#endif

    Log("[LIVEVIEW] Server stopped");
}

bool LiveviewServer::IsRunning() const
{
    return m_running.load();
}

int LiveviewServer::GetPort() const
{
    return m_port.load();
}

void LiveviewServer::PublishFrame(const void* pixels, int width, int height, int channels)
{
    if ((pixels == NULL) || (width <= 0) || (height <= 0) || (channels <= 0))
        return;

    size_t size = (size_t)width * (size_t)height * (size_t)channels;
    const uint8_t* bytes = (const uint8_t*)pixels;

    std::lock_guard<std::mutex> lock(m_snapshot_mutex);
    m_frame_pixels.assign(bytes, bytes + size);
    m_frame_width = width;
    m_frame_height = height;
    m_frame_channels = channels;
    m_frame_seq++;
}

void LiveviewServer::PublishStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(m_snapshot_mutex);

    if (status == m_status)
        return;

    m_status = status;
    m_status_seq++;
}

void LiveviewServer::ThreadFunc(void)
{
    std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_status = last_frame;

    while (m_running.load())
    {
        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(m_listen_socket, &read_set);
        int max_socket = (int)m_listen_socket;

        for (size_t i = 0; i < m_clients.size(); i++)
        {
            LiveviewClient* client = m_clients[i];
            FD_SET(client->socket, &read_set);

            if (client->send_offset < client->send_queue.size())
                FD_SET(client->socket, &write_set);

            if ((int)client->socket > max_socket)
                max_socket = (int)client->socket;
        }

        struct timeval timeout;
        timeout.tv_sec = LIVEVIEW_POLL_TIMEOUT_MS / 1000;
        timeout.tv_usec = (LIVEVIEW_POLL_TIMEOUT_MS % 1000) * 1000;

        int ready = select(max_socket + 1, &read_set, &write_set, NULL, &timeout);

        if (ready > 0)
        {
            for (size_t i = m_clients.size(); i > 0; i--)
            {
                size_t index = i - 1;
                LiveviewClient* client = m_clients[index];
                bool alive = true;

                if (FD_ISSET(client->socket, &write_set))
                    alive = FlushClient(client);

                if (alive && FD_ISSET(client->socket, &read_set))
                    alive = ReadClient(client);

                if (alive && client->close_after_flush &&
                    (client->send_offset >= client->send_queue.size()))
                {
                    alive = false;
                }

                if (!alive)
                    DropClient(index);
            }

            if (FD_ISSET(m_listen_socket, &read_set))
                AcceptClient();
        }

        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

        if (now - last_frame >= std::chrono::milliseconds(LIVEVIEW_FRAME_INTERVAL_MS))
        {
            last_frame = now;
            BroadcastFrame();
        }

        if (now - last_status >= std::chrono::milliseconds(LIVEVIEW_STATUS_INTERVAL_MS))
        {
            last_status = now;
            BroadcastStatus();
        }
    }

    CloseAllClients();
}

void LiveviewServer::AcceptClient(void)
{
    struct sockaddr_in client_address;
    memset(&client_address, 0, sizeof(client_address));
    liveview_socket_len_t client_length = sizeof(client_address);
    LiveviewSocket socket = accept(m_listen_socket, (struct sockaddr*)&client_address, &client_length);

    if (socket == LIVEVIEW_INVALID_SOCKET)
        return;

    if (!LiveviewSetNonBlocking(socket))
    {
        LIVEVIEW_SOCKET_CLOSE(socket);
        return;
    }

    LiveviewClient* client = new LiveviewClient();
    client->socket = socket;
    m_clients.push_back(client);
}

bool LiveviewServer::ReadClient(LiveviewClient* client)
{
    char buffer[4096];
    int received = (int)::recv(client->socket, buffer, sizeof(buffer), 0);

    if (received == 0)
        return false;

    if (received < 0)
        return LiveviewWouldBlock();

    if (client->websocket)
        return HandleWebsocketBytes(client, (const uint8_t*)buffer, (size_t)received);

    if (client->close_after_flush)
        return true;

    if (client->request.size() + (size_t)received > LIVEVIEW_MAX_REQUEST_SIZE)
    {
        QueueHttpResponse(client, "400 Bad Request", "text/plain", "400 Bad Request");
        client->close_after_flush = true;
        return true;
    }

    client->request.append(buffer, (size_t)received);

    return HandleRequest(client);
}

bool LiveviewServer::HandleRequest(LiveviewClient* client)
{
    size_t header_end = client->request.find("\r\n\r\n");

    if (header_end == std::string::npos)
        return true;

    size_t header_size = header_end + 4;
    size_t line_end = client->request.find("\r\n");
    std::string request_line = client->request.substr(0, line_end);
    std::string path;

    if (LiveviewExtractGetPath(request_line, path) && (path == "/ws"))
    {
        std::string response;
        LiveviewWsHandshakeStatus status = liveview_ws_handshake_response(client->request.c_str(),
            header_size, response);

        if (status != LIVEVIEW_WS_HANDSHAKE_OK)
        {
            QueueHttpResponse(client, "400 Bad Request", "text/plain", "400 Bad Request");
            client->close_after_flush = true;
            return true;
        }

        QueueBytes(client, response.c_str(), response.size());
        client->websocket = true;

        std::string pending = client->request.substr(header_size);
        client->request.clear();

        if (!pending.empty())
            return HandleWebsocketBytes(client, (const uint8_t*)pending.c_str(), pending.size());

        return true;
    }

    if (LiveviewExtractGetPath(request_line, path) && (path == "/"))
    {
        std::string page;
        {
            std::lock_guard<std::mutex> lock(m_page_mutex);
            page = m_page;
        }

        if (page.empty())
            page = k_liveview_placeholder_page;

        QueueHttpResponse(client, "200 OK", "text/html; charset=utf-8", page);
        client->close_after_flush = true;
        return true;
    }

    QueueHttpResponse(client, "404 Not Found", "text/plain", "404 Not Found");
    client->close_after_flush = true;

    return true;
}

bool LiveviewServer::HandleWebsocketBytes(LiveviewClient* client, const uint8_t* data, size_t size)
{
    client->decoder.Push(data, size);

    while (true)
    {
        LiveviewWsFrame frame;
        LiveviewWsDecodeStatus status = client->decoder.Next(frame);

        if (status == LIVEVIEW_WS_DECODE_NEED_MORE)
            return true;

        if (status == LIVEVIEW_WS_DECODE_PROTOCOL_ERROR)
        {
            Debug("[LIVEVIEW] WebSocket protocol error: %s", client->decoder.GetError());
            return false;
        }

        std::vector<uint8_t> out;

        if (frame.opcode == LIVEVIEW_WS_OPCODE_PING)
        {
            const void* payload = frame.payload.empty() ? NULL : (const void*)&frame.payload[0];
            liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_PONG, payload, frame.payload.size(), out);
            QueueBytes(client, out.empty() ? NULL : &out[0], out.size());
        }
        else if (frame.opcode == LIVEVIEW_WS_OPCODE_CLOSE)
        {
            liveview_ws_encode_close(1000, NULL, out);
            QueueBytes(client, out.empty() ? NULL : &out[0], out.size());
            client->close_after_flush = true;
            return true;
        }
    }
}

void LiveviewServer::QueueHttpResponse(LiveviewClient* client, const char* status_line,
    const char* content_type, const std::string& body)
{
    char length_text[32];
    snprintf(length_text, sizeof(length_text), "%d", (int)body.size());

    std::string response = "HTTP/1.1 ";
    response += status_line;
    response += "\r\nContent-Type: ";
    response += content_type;
    response += "\r\nContent-Length: ";
    response += length_text;
    response += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    response += body;

    QueueBytes(client, response.c_str(), response.size());
}

void LiveviewServer::QueueBytes(LiveviewClient* client, const void* data, size_t size)
{
    if ((data == NULL) || (size == 0))
        return;

    if (client->send_offset > 0)
    {
        client->send_queue.erase(client->send_queue.begin(),
            client->send_queue.begin() + (long)client->send_offset);
        client->send_offset = 0;
    }

    const uint8_t* bytes = (const uint8_t*)data;
    client->send_queue.insert(client->send_queue.end(), bytes, bytes + size);
}

bool LiveviewServer::FlushClient(LiveviewClient* client)
{
    while (client->send_offset < client->send_queue.size())
    {
        size_t pending = client->send_queue.size() - client->send_offset;
        int sent = (int)::send(client->socket, (const char*)&client->send_queue[client->send_offset],
            (int)pending, LIVEVIEW_SEND_FLAGS);

        if (sent > 0)
        {
            client->send_offset += (size_t)sent;
            continue;
        }

        return (sent < 0) && LiveviewWouldBlock();
    }

    client->send_queue.clear();
    client->send_offset = 0;

    return true;
}

void LiveviewServer::DropClient(size_t index)
{
    LiveviewClient* client = m_clients[index];

    if (client->socket != LIVEVIEW_INVALID_SOCKET)
    {
        LIVEVIEW_SOCKET_SHUTDOWN(client->socket);
        LIVEVIEW_SOCKET_CLOSE(client->socket);
    }

    delete client;
    m_clients.erase(m_clients.begin() + (long)index);
}

void LiveviewServer::CloseAllClients(void)
{
    while (!m_clients.empty())
        DropClient(m_clients.size() - 1);
}

void LiveviewServer::BroadcastFrame(void)
{
    bool has_listener = false;

    for (size_t i = 0; i < m_clients.size(); i++)
    {
        if (m_clients[i]->websocket && !m_clients[i]->close_after_flush)
        {
            has_listener = true;
            break;
        }
    }

    if (!has_listener)
        return;

    int width = 0;
    int height = 0;
    int channels = 0;

    {
        std::lock_guard<std::mutex> lock(m_snapshot_mutex);

        if ((m_frame_seq == m_last_frame_seq) || m_frame_pixels.empty())
            return;

        m_encode_pixels = m_frame_pixels;
        width = m_frame_width;
        height = m_frame_height;
        channels = m_frame_channels;
        m_last_frame_seq = m_frame_seq;
    }

    std::vector<uint8_t> png;

    if (stbi_write_png_to_func(LiveviewPngWriter, &png, width, height, channels,
        &m_encode_pixels[0], width * channels) == 0)
    {
        Debug("[LIVEVIEW] PNG encoding failed for %dx%d", width, height);
        return;
    }

    if (png.empty())
        return;

    std::vector<uint8_t> frame;
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_BINARY, &png[0], png.size(), frame);
    BroadcastToClients(frame, true);
}

void LiveviewServer::BroadcastStatus(void)
{
    std::string status;

    {
        std::lock_guard<std::mutex> lock(m_snapshot_mutex);

        if ((m_status_seq == m_last_status_seq) || m_status.empty())
            return;

        status = m_status;
        m_last_status_seq = m_status_seq;
    }

    std::vector<uint8_t> frame;
    liveview_ws_encode_frame(LIVEVIEW_WS_OPCODE_TEXT, status.c_str(), status.size(), frame);
    BroadcastToClients(frame, false);
}

void LiveviewServer::BroadcastToClients(const std::vector<uint8_t>& frame, bool droppable)
{
    if (frame.empty())
        return;

    for (size_t i = m_clients.size(); i > 0; i--)
    {
        size_t index = i - 1;
        LiveviewClient* client = m_clients[index];

        if (!client->websocket || client->close_after_flush)
            continue;

        size_t pending = client->send_queue.size() - client->send_offset;

        // A client that is still receiving the previous image misses this one
        // instead of growing a queue of stale images.
        if (droppable && (pending > 0))
            continue;

        if (pending + frame.size() > LIVEVIEW_MAX_SEND_QUEUE_SIZE)
        {
            Debug("[LIVEVIEW] Dropping a client that is too slow to receive");
            DropClient(index);
            continue;
        }

        QueueBytes(client, &frame[0], frame.size());

        if (!FlushClient(client))
            DropClient(index);
    }
}
