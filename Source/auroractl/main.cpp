/*
 * Created by v1tr10l7 on 27.01.2026.
 * Copyright (c) 2024-2026, Szymon Zemke <v1tr10l7@proton.me>
 *
 * SPDX-License-Identifier: GPL-3
 */
#include <Prism/Containers/Vector.hpp>
#include <Prism/Core/Error.hpp>
#include <Prism/Debug/Log.hpp>
#include <Prism/String/String.hpp>

using namespace Prism;

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <memory.h>

ErrorOr<void> NeonMain(const Vector<StringView>& args,
                       const Vector<StringView>& envp)
{
    if (args.Size() < 2)
    {
        Log::Logf(LogLevel::eError, "Usage: %s <command>\n", args[0].Raw());
        return Error(EINVAL);
    }
    const char* socket_path = "/run/auroractl.sock";

    // 2. Create the Socket (AF_UNIX for local communication)
    int         sock        = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
    {
        perror("socket error");
        return Error(errno);
    }

    // 3. Set up the address structure
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    // 4. Connect to the socket
    printf("call before connect, sockFd: %d\n", sock);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        perror("connect error (is the server running?)");
        close(sock);
        return Error(errno);
    }

    // 5. Send the command
    auto command = args[1];
    printf("call before send, sockFd: %d\n", sock);
    if (send(sock, command.Raw(), command.Size(), 0) == -1)
    {
        perror("send error");
        close(sock);
        return Error(errno);
    }

    Log::Logf(LogLevel::eTrace, "Sent: %s\n", command.Raw());

    // 6. Receive response (Optional but recommended)
    char    buffer[1024];
    ssize_t bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received > 0)
    {
        buffer[bytes_received] = '\0';
        Log::Logf(LogLevel::eTrace, "Response: %s\n", buffer);
    }

    close(sock);
    return {};
}
