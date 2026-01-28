/*
 * Created by v1tr10l7 on 02.09.2025.
 * Copyright (c) 2024-2025, Szymon Zemke <v1tr10l7@proton.me>
 *
 * SPDX-License-Identifier: GPL-3
 */
#include <Neon/Core/Environment.hpp>
#include <Neon/Filesystem/File.hpp>
#include <Neon/Filesystem/Filesystem.hpp>
#include <Neon/Process/Process.hpp>

#include <Prism/Debug/Assertions.hpp>
#include <Prism/Debug/Log.hpp>
#include <Prism/Utility/Atomic.hpp>

#include <cryptix/syscall.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Prism;
using namespace Neon;

#define Assert(...) assert(__VA_ARGS__)

#define DeclareLogNamed(name, level)                                           \
    void name(const char* format, ...)                                         \
    {                                                                          \
        VaList args;                                                           \
        PrismVaStart(args, format);                                            \
        Log::Logv(LogLevel::e##level, format, args);                           \
        PrismVaEnd(args);                                                      \
    }
#define DeclareLog(level) DeclareLogNamed(level, level)

DeclareLog(Trace);
DeclareLog(Info);
DeclareLog(Debug);
DeclareLog(Warn);
DeclareLogNamed(Message, None);
DeclareLogNamed(OnError, Error);

namespace
{
    Atomic<bool> s_IsSystemManager    = false;
    sigset_t     s_OriginalSignalMask = {};

    Ref<File>    s_StdIn              = nullptr;
    Ref<File>    s_StdOut             = nullptr;
    Ref<File>    s_StdErr             = nullptr;
}; // namespace

ErrorOr<void> initializeStdIo()
{
    Trace("Aurora: Opening stdio streams...");
    auto stdIn  = TryOrRet(File::Open("/dev/tty1"_sv, FileOpenFlags::eRead));
    auto stdOut = TryOrRet(File::Open("/dev/tty1"_sv, FileOpenFlags::eWrite));
    auto stdErr = TryOrRet(File::Open("/dev/tty1"_sv, FileOpenFlags::eWrite));

    if (auto result = stdIn->Duplicate(0); result) s_StdIn = *result;
    if (auto result = stdOut->Duplicate(1); result) s_StdOut = *result;
    if (auto result = stdErr->Duplicate(2); result) s_StdErr = *result;

    Info("Aurora: stdin.open: %i, stdout.open: %i, stderr.open: %i",
         s_StdIn.operator bool(), s_StdOut.operator bool(),
         s_StdErr.operator bool());
    return {};
}
ErrorOr<isize> mountFilesystems()
{
    auto result = Process::Spawn("/bin/mount"_sv, {"/bin/mount"_sv, "-a"_sv});

    if (!result) OnError("Aurora: Failed to execute /bin/mount -a");
    auto process = *result;

    int  status  = 0;
    process->Wait(status, 0);

    if (auto result
        = Filesystem::CreateDirectory("/tmp"_sv, static_cast<FileMode>(0755));
        !result && result.Error() != EEXIST)
        OnError("Aurora: Failed to create /tmp directory: %s",
                strerror(result.Error()));
    if (auto result
        = Filesystem::CreateDirectory("/run"_sv, static_cast<FileMode>(0755));
        !result && result.Error() != EEXIST)
        OnError("Aurora: Failed to create /run directory: %s",
                strerror(result.Error()));

    return status;
}

static void signalHandler(int signo)
{
    Info("Aurora: Received %ld signal", i64(signo));
}
static void handleShutdown(i32 signo)
{
    Info("Aurora: Received shutdown signal #%ld", i64(signo));
    // TODO(v1tr10l7): shutdown all services once we have them

    Neon::Filesystem::SyncAll();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
}

static ErrorOr<void> setupSignals()
{
    sigset_t sigwaitSet;
    if (s_IsSystemManager)
        // Block all signals in system manager mode - don't want to chance
        // provoking a signal that will suspend or terminate the process
        sigfillset(&sigwaitSet);
    else {
        sigemptyset(&sigwaitSet);
        sigaddset(&sigwaitSet, SIGCHLD);
        sigaddset(&sigwaitSet, SIGINT);
        sigaddset(&sigwaitSet, SIGTERM);
        sigaddset(&sigwaitSet, SIGUSR1);
    }
    sigprocmask(SIG_BLOCK, &sigwaitSet, &s_OriginalSignalMask);

    // Terminal access control signals - we ignore these so that dinit can't be
    // suspended if it writes to the terminal after some other process has
    // claimed ownership of it.
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    signal(SIGPIPE, SIG_IGN);

    struct sigaction psa;
    psa.sa_handler = signalHandler;
    sigaction(SIGHUP, &psa, nullptr);

    return {};
    psa.sa_handler = handleShutdown;
    sigaction(SIGINT, &psa, nullptr);
    sigaction(SIGTERM, &psa, nullptr);
}

static ErrorOr<isize> setupCommandSocket()
{
    constexpr StringView SOCKET_PATH = "/run/auroractl.sock";
    isize                fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

    sockaddr_un          addr{};
    addr.sun_family = AF_UNIX;
    SOCKET_PATH.Copy(reinterpret_cast<char*>(addr.sun_path),
                     sizeof(addr.sun_path) - 1);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
    {
        OnError("Aurora: Failed to bind socket: '%s', errno: %s",
                SOCKET_PATH.Raw(), strerror(errno));
        return Error(errno);
    }

    listen(fd, 5);
    chmod(SOCKET_PATH.Raw(), 0666);
    return fd;
}

ErrorOr<void> NeonMain(const Vector<StringView>& argv,
                       const Vector<StringView>& envp)
{
    Assert(initializeStdIo());
    Trace("Aurora: Initializing...");
    Debug("Aurora: ProcessID => %i", getpid());
    Debug("Aurora: Arguments => ");
    for (usize i = 0; const auto arg : argv)
        Message("\tArgs[%zu]: %s", i++, arg);
    for (usize i = 0; const auto env : envp)
        Message("\tEnvs[%zu]: %s", i++, env);

    Trace("Aurora: Setting up environment variables");

#ifdef __cryptix__
    Environment::Overwrite("TERM"_sv, "linux"_sv);
    Environment::Overwrite("USER"_sv, "root"_sv);
    Environment::Overwrite("HOME"_sv, "/root"_sv);
    Environment::Overwrite("PATH"_sv, "/usr/local/bin:/ur/bin:/usr/sbin"_sv);

    Message("\n\n\n\n");
    Info("Aurora: Welcome to CryptixOS!");
#endif

    mountFilesystems();
    static constexpr PathView shellPath = "/usr/bin/bash"_sv;
    if (!Filesystem::Access(shellPath, FileMode::eExecute))
    {
        OnError("Aurora: Failed to access the shell => %s", shellPath);
        return Error(errno);
    }

    if (auto status = setupSignals(); !status)
    {
        auto        errCode        = status.Error();
        const char* errCodeString  = StringUtils::ToString(errCode).Raw();
        const char* errDescription = strerror(status.Error());
        OnError("Aurora: failed to setup signals => %s: %s",
                errCodeString ?: "(null)", errDescription ?: "(null)");
    }
    ProcessID self = Process::CurrentID();
    Trace("Aurora: Sending %d signal to pid #%d", SIGHUP, self);
    kill(self, SIGHUP);

    Info("Aurora: Pivoting root...");
    mkdir("/mnt/ext2/tmp", 0755);
    if (auto result = Filesystem::PivotRoot("/mnt/ext2", "/mnt/ext2/tmp");
        !result)
        OnError("Aurora: Pivot root failed: %s", strerror(result.Error()));

    // i32         fd = open("/etc/passwd", O_RDONLY);
    //
    // struct stat st;
    // fstat(fd, &st);
    // char* passwd = reinterpret_cast<char*>(
    //     mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    // if (passwd == MAP_FAILED) OnError("Aurora: Failed to mmap /etc/passwd");
    // else Trace("Aurora: Dumping /etc/passwd: %s", passwd);

    isize cmdSocket = -1;
    if (auto ret = setupCommandSocket(); ret) cmdSocket = *ret;

    for (;;)
    {
        // 1. Check for incoming commands (Non-blocking)
        isize clientFd
            = cmdSocket != -1 ? accept(cmdSocket, nullptr, nullptr) : -1;
        if (clientFd != -1)
        {
            char buffer[1024];
            int  n = read(clientFd, buffer, sizeof(buffer) - 1);
            if (n > 0)
            {
                buffer[n] = '\0';
                StringView command{buffer};
                Info("Aurora: Received socket command: %s", buffer);

                // Handle commands
                if (command == "reboot")
                {
                    write(clientFd, "Rebooting...", 12);
                    handleShutdown(SIGTERM);
                }
                else write(clientFd, "Unknown Command", 15);
            }
            close(clientFd);
        }

        Trace("Aurora: Launching shell...");
        Filesystem::ChangeDirectory(Environment::Get("HOME"_sv));
        auto result = Process::Spawn(shellPath, {shellPath.Raw(), "-i"});
        if (!result)
        {
            OnError("Aurora: fork failed");
            return Error(errno);
        }

        auto process = *result;
        auto pid     = process->ID();

        i32  status;
    continue_waiting:
        if (process->Wait(status, 0) == pid)
        {
            bool exited = WIFEXITED(status);
            if (!exited) goto continue_waiting;

            Info("Aurora: Child %d died with exit code %d", pid,
                 WEXITSTATUS(status));
        }
        usleep(10000);
    }

    Trace("Aurora: Exiting...");
    return {};
}
