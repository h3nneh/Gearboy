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

// The live view server owns the only stb_image_write implementation of this
// binary, exactly as emu.cpp does in the emulator build.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "liveview/liveview_server.h"
#include "liveview/liveview_ws.h"

bool g_mcp_stdio_mode = true;

static const uint8_t k_png_signature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

// Counts the threads of this process, so a leaked server thread is visible.
static int CountThreads(void)
{
    DIR* directory = opendir("/proc/self/task");

    if (directory == NULL)
        return -1;

    int count = 0;
    struct dirent* entry = readdir(directory);

    while (entry != NULL)
    {
        if (entry->d_name[0] != '.')
            count++;

        entry = readdir(directory);
    }

    closedir(directory);

    return count;
}

static int CountOpenFiles(void)
{
    DIR* directory = opendir("/proc/self/fd");

    if (directory == NULL)
        return -1;

    int count = 0;
    struct dirent* entry = readdir(directory);

    while (entry != NULL)
    {
        if (entry->d_name[0] != '.')
            count++;

        entry = readdir(directory);
    }

    closedir(directory);

    return count;
}

static int ConnectToPort(int port)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    Check(socket_fd >= 0, "client socket creation");

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    Check(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1, "client address");

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(socket_fd, (struct sockaddr*)&address, sizeof(address)) != 0)
    {
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void SendAll(int socket_fd, const void* data, size_t size)
{
    const char* bytes = (const char*)data;
    size_t sent_total = 0;

    while (sent_total < size)
    {
        ssize_t sent = send(socket_fd, bytes + sent_total, size - sent_total, 0);
        Check(sent > 0, "client send");
        sent_total += (size_t)sent;
    }
}

// Reads until the connection is closed by the server.
static std::string ReadUntilClose(int socket_fd)
{
    std::string result;
    char buffer[4096];

    while (true)
    {
        ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);

        if (received <= 0)
            break;

        result.append(buffer, (size_t)received);
    }

    return result;
}

// Reads exactly size bytes, failing the test on timeout.
static void ReadExactly(int socket_fd, size_t size, std::vector<uint8_t>& out)
{
    out.clear();

    while (out.size() < size)
    {
        uint8_t buffer[4096];
        size_t missing = size - out.size();
        size_t chunk = missing < sizeof(buffer) ? missing : sizeof(buffer);
        ssize_t received = recv(socket_fd, buffer, chunk, 0);
        Check(received > 0, "client read timed out or connection closed");
        out.insert(out.end(), buffer, buffer + received);
    }
}

static std::string ReadHttpHeaders(int socket_fd)
{
    std::string headers;

    while (headers.find("\r\n\r\n") == std::string::npos)
    {
        char buffer[1024];
        ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
        Check(received > 0, "http header read timed out or connection closed");
        headers.append(buffer, (size_t)received);
    }

    return headers;
}

// Reads one unmasked server frame.
static uint8_t ReadServerFrame(int socket_fd, std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> header;
    ReadExactly(socket_fd, 2, header);

    uint8_t opcode = (uint8_t)(header[0] & 0x0F);
    Check((header[1] & 0x80) == 0, "server frames must not be masked");

    size_t size = (size_t)(header[1] & 0x7F);

    if (size == 126)
    {
        std::vector<uint8_t> extended;
        ReadExactly(socket_fd, 2, extended);
        size = ((size_t)extended[0] << 8) | (size_t)extended[1];
    }
    else if (size == 127)
    {
        std::vector<uint8_t> extended;
        ReadExactly(socket_fd, 8, extended);
        size = 0;

        for (int i = 0; i < 8; i++)
            size = (size << 8) | (size_t)extended[i];
    }

    payload.clear();

    if (size > 0)
        ReadExactly(socket_fd, size, payload);

    return opcode;
}

// Builds a masked client frame, as a browser sends them.
static std::vector<uint8_t> MakeClientFrame(uint8_t opcode, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    frame.push_back((uint8_t)(0x80 | opcode));

    Check(payload.size() < 126, "test client only builds short frames");
    frame.push_back((uint8_t)(0x80 | payload.size()));

    const uint8_t mask[4] = { 0x11, 0x22, 0x33, 0x44 };

    for (int i = 0; i < 4; i++)
        frame.push_back(mask[i]);

    for (size_t i = 0; i < payload.size(); i++)
        frame.push_back((uint8_t)(payload[i] ^ mask[i % 4]));

    return frame;
}

static int OpenWebsocket(int port, const std::string& key)
{
    int socket_fd = ConnectToPort(port);
    Check(socket_fd >= 0, "websocket connect");

    std::string request =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    SendAll(socket_fd, request.c_str(), request.size());

    std::string headers = ReadHttpHeaders(socket_fd);
    Check(headers.compare(0, 12, "HTTP/1.1 101") == 0, "websocket upgrade answers 101");

    std::string accept;
    Check(liveview_ws_accept_key(key, accept), "accept key computation");
    Check(headers.find("Sec-WebSocket-Accept: " + accept + "\r\n") != std::string::npos,
        "websocket upgrade carries the accept key");

    return socket_fd;
}

// Start on an ephemeral port comes up and shuts down again, repeatedly, without
// leaking threads or sockets.
static void TestStartStopLifecycle(void)
{
    int threads_before = CountThreads();
    int files_before = CountOpenFiles();

    for (int i = 0; i < 5; i++)
    {
        LiveviewServer server;
        Check(server.Start("127.0.0.1", 0), "start on an ephemeral port");
        Check(server.IsRunning(), "server reports itself running");
        Check(server.GetPort() > 0, "ephemeral port is reported back");
        Check(!server.Start("127.0.0.1", 0), "a running server does not start twice");
        server.Stop();
        Check(!server.IsRunning(), "server reports itself stopped");
        Check(server.GetPort() == 0, "port is cleared on stop");
        server.Stop();
    }

    {
        LiveviewServer server;
        Check(!server.Start("256.256.256.256", 0), "an invalid address does not start");
        Check(!server.IsRunning(), "a failed start leaves no running server");
        Check(!server.Start("127.0.0.1", 70000), "an invalid port does not start");
    }

    int threads_after = CountThreads();
    int files_after = CountOpenFiles();

    Check(threads_before > 0 && threads_after > 0, "thread counting works");
    Check(threads_after == threads_before, "repeated start and stop leaks no threads");
    Check(files_before > 0 && files_after > 0, "file descriptor counting works");
    Check(files_after == files_before, "repeated start and stop leaks no sockets");
}

// GET / serves the page, everything else answers 404.
static void TestHttpRoutes(void)
{
    LiveviewServer server;
    Check(server.Start("127.0.0.1", 0), "start for http routes");
    server.SetPage("<!DOCTYPE html><html><body>gearboy live view</body></html>");

    int socket_fd = ConnectToPort(server.GetPort());
    Check(socket_fd >= 0, "connect for GET /");
    const char* root_request = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    SendAll(socket_fd, root_request, strlen(root_request));
    std::string response = ReadUntilClose(socket_fd);
    close(socket_fd);

    Check(response.compare(0, 12, "HTTP/1.1 200") == 0, "GET / answers 200");
    Check(response.find("Content-Type: text/html") != std::string::npos,
        "GET / answers with an html content type");
    Check(response.find("gearboy live view</body></html>") != std::string::npos,
        "GET / answers with the page set from outside");

    socket_fd = ConnectToPort(server.GetPort());
    Check(socket_fd >= 0, "connect for GET /nothing");
    const char* unknown_request = "GET /nothing HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    SendAll(socket_fd, unknown_request, strlen(unknown_request));
    response = ReadUntilClose(socket_fd);
    close(socket_fd);

    Check(response.compare(0, 12, "HTTP/1.1 404") == 0, "an unknown path answers 404");

    server.Stop();
}

// A connected client receives published frames as PNG binary frames and
// published status as text frames, and its pings are answered.
static void TestWebsocketStream(void)
{
    LiveviewServer server;
    Check(server.Start("127.0.0.1", 0), "start for the websocket stream");

    int socket_fd = OpenWebsocket(server.GetPort(), "dGhlIHNhbXBsZSBub25jZQ==");

    const int width = 16;
    const int height = 8;
    std::vector<uint8_t> pixels((size_t)(width * height * 3));

    for (size_t i = 0; i < pixels.size(); i++)
        pixels[i] = (uint8_t)(i * 7);

    server.PublishFrame(&pixels[0], width, height, 3);

    std::vector<uint8_t> payload;
    uint8_t opcode = ReadServerFrame(socket_fd, payload);

    Check(opcode == LIVEVIEW_WS_OPCODE_BINARY, "a published frame arrives as a binary frame");
    Check(payload.size() > sizeof(k_png_signature), "the binary frame carries an image");
    Check(memcmp(&payload[0], k_png_signature, sizeof(k_png_signature)) == 0,
        "the binary frame starts with the png signature");

    std::vector<uint8_t> ping_payload;
    ping_payload.push_back('h');
    ping_payload.push_back('i');
    std::vector<uint8_t> ping = MakeClientFrame(LIVEVIEW_WS_OPCODE_PING, ping_payload);
    SendAll(socket_fd, &ping[0], ping.size());

    // The server may still be sending images, the pong follows them.
    for (int i = 0; i < 20; i++)
    {
        opcode = ReadServerFrame(socket_fd, payload);

        if (opcode == LIVEVIEW_WS_OPCODE_PONG)
            break;

        Check(opcode == LIVEVIEW_WS_OPCODE_BINARY, "only images arrive before the pong");
    }

    Check(opcode == LIVEVIEW_WS_OPCODE_PONG, "a client ping is answered with a pong");
    Check(payload.size() == 2 && payload[0] == 'h' && payload[1] == 'i',
        "the pong carries the ping payload");

    server.PublishStatus("{\"type\":\"status\"}");

    for (int i = 0; i < 20; i++)
    {
        opcode = ReadServerFrame(socket_fd, payload);

        if (opcode == LIVEVIEW_WS_OPCODE_TEXT)
            break;

        Check(opcode == LIVEVIEW_WS_OPCODE_BINARY, "only images arrive before the status");
    }

    Check(opcode == LIVEVIEW_WS_OPCODE_TEXT, "a published status arrives as a text frame");
    Check(std::string((const char*)&payload[0], payload.size()) == "{\"type\":\"status\"}",
        "the text frame carries the published status");

    close(socket_fd);
    server.Stop();
}

int main(void)
{
    TestStartStopLifecycle();
    TestHttpRoutes();
    TestWebsocketStream();

    printf("All live view server tests passed\n");

    return 0;
}
