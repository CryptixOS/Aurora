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

#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

using namespace Prism;
void Info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Log::Logv(LogLevel::eInfo, format, args);
    va_end(args);
}
void Trace(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Log::Logv(LogLevel::eTrace, format, args);
    va_end(args);
}
void OnError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Log::Logv(LogLevel::eError, format, args);
    va_end(args);
}
void Message(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    Log::Logv(LogLevel::eNone, format, args);
    va_end(args);
}

constexpr usize MAX_LINE_LENGTH = 1024;
constexpr usize MAX_FIELDS      = 6;

struct MountPoint
{
    char* Source;         // Device or server
    char* Target;         // Mount point
    char* FilesystemType; // Filesystem type
    char* Options;        // Mount options
    i32   Frequency;      // Dump frequency
    i32   FsckPassNumber; // Fsck pass number
};

void trimNewLine(char* str)
{
    usize len = strlen(str);
    if (len && str[len - 1] == '\n') str[len - 1] = '\0';
}
usize parseMountOptions(char*& opts)
{
    usize flags = 0;
    if (!opts) return 0;

    char* options = strdup(opts);
    char* token   = strtok(options, ",");

    char* current = opts;
    while (token)
    {
        if (!strcmp(token, "ro")) flags |= MS_RDONLY;
        else if (!strcmp(token, "noatime")) flags |= MS_NOATIME;
        else if (!strcmp(token, "relatime")) flags |= MS_RELATIME;
        else if (!strcmp(token, "nosuid")) flags |= MS_NOSUID;
        else if (!strcmp(token, "nodev")) flags |= MS_NODEV;
        else if (!strcmp(token, "noexec")) flags |= MS_NOEXEC;
        else if (!strcmp(token, "sync")) flags |= MS_SYNCHRONOUS;
        else if (!strcmp(token, "dirsync")) flags |= MS_DIRSYNC;
        else strcpy(current, token);

        current += strlen(token);
        token = strtok(NULL, ",");
    }

    delete options;
    *current = 0;
    return flags;
}

i32 parseMountPoint(const char* line, MountPoint& entry)
{
    char* fields[MAX_FIELDS] = {0};
    char* temp               = strdup(line);
    char* token              = strtok(temp, " \t");
    i32   fieldCount         = 0;

    while (token && fieldCount < static_cast<i32>(MAX_FIELDS))
    {
        fields[fieldCount++] = token;
        token                = strtok(NULL, " \t");
    }

    if (fieldCount < 4)
    {
        delete temp;
        return 0;
    }

    entry.Source         = strdup(fields[0]);
    entry.Target         = strdup(fields[1]);
    entry.FilesystemType = strdup(fields[2]);
    entry.Options        = strdup(fields[3]);
    entry.Frequency      = (fieldCount >= 5) ? atoi(fields[4]) : 0;
    entry.FsckPassNumber = (fieldCount >= 6) ? atoi(fields[5]) : 0;

    delete temp;
    return 1;
}

void freeMountPoint(MountPoint& entry)
{
    delete entry.Source;
    delete entry.Target;
    delete entry.FilesystemType;
    delete entry.Options;
}

isize mountFilesystems()
{
    FILE* file = fopen("/etc/fstab", "r");
    if (!file)
    {
        OnError("Failed to open /etc/fstab");
        return EXIT_FAILURE;
    }

    char line[MAX_LINE_LENGTH];
    i32  lineNumber = 0;

    while (fgets(line, sizeof(line), file))
    {
        lineNumber++;
        trimNewLine(line);

        // Skip comments and empty lines
        if (line[0] == '#' || strlen(line) == 0) continue;

        MountPoint entry;
        if (parseMountPoint(line, entry))
        {
            usize mountFlags = parseMountOptions(entry.Options);
            Trace("Aurora: Mount options: %s", entry.Options);
            i32 mountStatus
                = mount(entry.Source, entry.Target, entry.FilesystemType,
                        mountFlags, entry.Options);
            Trace("Aurora: Mount status: %d", mountStatus);
            if (mountStatus == -1)
                OnError(
                    "Aurora: Failed to mount `%s` filesystem at `%s`, "
                    "source: %s, flags: `%s`\nerror code: %s",
                    entry.FilesystemType, entry.Target, entry.Source,
                    entry.Options, strerror(errno));

            freeMountPoint(entry);
        }
        else
            OnError("Aurora: Skipping invalid or incomplete line %d",
                    lineNumber);
    }

    fclose(file);
    return EXIT_SUCCESS;
}

static void signalHandler(int signo)
{
    Info("Aurora: Received %d signal", signo);
}

int NeonMain(const Vector<StringView>& argv, const Vector<StringView>& envp)
{
    Trace("Aurora: Setting up environment variables");
    using namespace Neon;

#ifdef __cryptix__
    Environment::Overwrite("TERM", "linux");
    Environment::Overwrite("USER", "root");
    Environment::Overwrite("HOME", "/root");
    Environment::Overwrite("PATH", "/usr/local/bin:/ur/bin:/usr/sbin");

    Message("Aurora: Welcome to CryptixOS!");
#endif

    mountFilesystems();
    static constexpr PathView shellPath = "/usr/bin/bash"_sv;
    if (!Filesystem::Access(shellPath, FileMode::eExecute))
    {
        OnError("Aurora: Failed to access the shell => %s", shellPath);
        return EXIT_FAILURE;
    }

    struct sigaction psa;
    psa.sa_handler = signalHandler;
    sigaction(SIGHUP, &psa, nullptr);

    pid_t self = getpid();
    kill(self, SIGHUP);

    for (;;)
    {
        Trace("Aurora: Launching shell...");

        i32 pid = fork();
        if (pid == -1)
        {
            OnError("Aurora: fork failed");
            return EXIT_FAILURE;
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
    return EXIT_SUCCESS;
}
