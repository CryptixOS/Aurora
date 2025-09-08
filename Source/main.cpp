/*
 * Created by v1tr10l7 on 02.09.2025.
 * Copyright (c) 2024-2025, Szymon Zemke <v1tr10l7@proton.me>
 *
 * SPDX-License-Identifier: GPL-3
 */
#include <Neon/Core/Environment.hpp>
#include <Neon/Filesystem/File.hpp>
#include <Neon/Filesystem/Filesystem.hpp>

#include <Prism/Debug/Assertions.hpp>
#include <Prism/Debug/Log.hpp>
#include <Prism/Utility/Atomic.hpp>

#include <cryptix/syscall.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Prism;
#define Assert(...) PrismAssert(__VA_ARGS__)

#define DeclareLogNamed(name, level)                                           \
    void name(const char* format, ...)                                         \
    {                                                                          \
        va_list args;                                                          \
        va_start(args, format);                                                \
        Log::Logv(LogLevel::e##level, format, args);                           \
        va_end(args);                                                          \
    }
#define DeclareLog(level) DeclareLogNamed(level, level)

DeclareLog(Trace);
DeclareLog(Info);
DeclareLog(Debug);
DeclareLog(Warn);
DeclareLogNamed(Message, None);
DeclareLogNamed(OnError, Error);

ErrorOr<void> initializeStdIo()
{
    return {};

    i32 onefd = open("/dev/console", O_RDONLY, 0);
    if (onefd != -1) dup2(onefd, 0);
    i32 twofd = open("/dev/console", O_RDWR, 0);
    if (twofd != -1)
    {
        dup2(twofd, 1);
        dup2(twofd, 2);
    }

    if (onefd > 2) close(onefd);
    if (twofd > 2) close(twofd);

    if (errno) return Error(errno);
    return {};
}
ErrorOr<isize> mountFilesystems()
{
    int pid = fork();
    if (pid == -1) return Error(errno);
    else if (pid == 0)
    {
        execl("/bin/mount", "mount", "-a", nullptr);
        OnError("Aurora: Failed to execute /bin/mount -a");
        return Error(errno);
    }

    int status = -1;
    waitpid(pid, &status, 0);

    if (errno) return Error(errno);
    return status;
}

namespace mlibc
{
    extern void sys_sigreturn();
}

static void signalHandler(int signo)
{
    Info("Aurora: Received %ld signal", i64(signo));
}

ErrorOr<void> NeonMain(const Vector<StringView>& argv,
                       const Vector<StringView>& envp)
{
    Trace("Aurora: Ininitializing...");
    Debug("Aurora: Arguments => ");
    for (usize i = 0; const auto arg : argv)
        Message("\tArgs[%zu]: %s", i++, arg);
    Debug("Aurora: Environment variables => ");
    for (usize i = 0; const auto env : envp)
        Message("\tEnvs[%zu]: %s", i++, env);

    Trace("Aurora: Setting up environment variables");
    using namespace Neon;

#ifdef __cryptix__
    Environment::Overwrite("TERM", "linux");
    Environment::Overwrite("USER", "root");
    Environment::Overwrite("HOME", "/root");
    Environment::Overwrite("PATH", "/usr/local/bin:/ur/bin:/usr/sbin");

    Message("\n\n\n\n");
    Info("Aurora: Welcome to CryptixOS!");
#endif

    Assert(initializeStdIo());
    mountFilesystems();
    static constexpr PathView shellPath = "/usr/bin/bash"_sv;
    if (!Filesystem::Access(shellPath, FileMode::eExecute))
    {
        OnError("Aurora: Failed to access the shell => %s", shellPath);
        return Error(errno);
    }

    struct sigaction psa;
    psa.sa_handler = signalHandler;
    sigaction(SIGHUP, &psa, nullptr);

    pid_t self = getpid();
    Trace("Aurora: Sending %d signal to pid #%d", SIGHUP, self);
    kill(self, SIGHUP);

    for (;;)
    {
        Trace("Aurora: Launching shell...");

        i32 pid = fork();
        if (pid == -1)
        {
            OnError("Aurora: fork failed");
            return Error(errno);
        }
        else if (pid == 0)
        {
            const char* flag = "-i";
            char* const argv[]
                = {(char*)shellPath.Raw(), const_cast<char*>(flag), NULL};
            Filesystem::ChangeDirectory(Environment::Get("HOME"));
            execvp(shellPath.Raw(), argv);
        }

        i32 status;
    continue_waiting:
        if (waitpid(pid, &status, 0) == pid)
        {
            bool exited = WIFEXITED(status);
            if (!exited) goto continue_waiting;

            Info("Aurora: Child %d died with exit code %d", pid,
                 WEXITSTATUS(status));
        }
    }

    Trace("Aurora: Exiting...");
    return {};
}
