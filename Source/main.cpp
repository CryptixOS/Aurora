/*
 * Created by v1tr10l7 on 02.09.2025.
 * Copyright (c) 2024-2025, Szymon Zemke <v1tr10l7@proton.me>
 *
 * SPDX-License-Identifier: GPL-3
 */
#include <Prism/Debug/Assertions.hpp>
#include <Prism/Debug/Log.hpp>

#include <sys/mount.h>
#include <unistd.h>
#include <wait.h>

using namespace Prism;
using Log::Error;
using Log::Info;
using Log::Trace;

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

    while (token && fieldCount < MAX_FIELDS)
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
        perror("Failed to open /etc/fstab");
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
            printf("options: %s\n", entry.Options);
            i32 mountStatus
                = mount(entry.Source, entry.Target, entry.FilesystemType,
                        mountFlags, entry.Options);
            printf("mount status: %d\n", mountStatus);
            if (mountStatus == -1)
                fprintf(stderr,
                        "init: failed to mount `%s` filesystem at `%s`, "
                        "source: %s, flags: `%s`\nerror code: %s\n",
                        entry.FilesystemType, entry.Target, entry.Source,
                        entry.Options, strerror(errno));

            freeMountPoint(entry);
        }
        else
            fprintf(stderr, "Skipping invalid or incomplete line %d\n",
                    lineNumber);
    }

    fclose(file);
    return EXIT_SUCCESS;
}

i32 main()
{
    Trace("Aurora: Setting up environment variables");
#ifdef __cryptix__
    setenv("TERM", "linux", 1);
    setenv("USER", "root", 1);
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/bin:/ur/bin:/usr/sbin", 1);

    Log::Log(LogLevel::eNone, "Aurora: Welcome to CryptixOS!\n");
#endif

    PrismAssert(mountFilesystems() == 0);
    static constexpr StringView shellPath = "/usr/bin/bash"_sv;
    if (access(shellPath.Raw(), X_OK) == -1)
    {
        Error("Aurora: Failed to access the shell => {}", shellPath);
        return EXIT_FAILURE;
    }

    for (;;)
    {
        Trace("Aurora: Launching shell...");

        i32 pid = fork();
        if (pid == -1)
        {
            Error("Aurora: fork failed");
            return EXIT_FAILURE;
        }
        else if (pid == 0)
        {
            const char* flag = "-i";
            char* const argv[]
                = {(char*)shellPath.Raw(), const_cast<char*>(flag), NULL};
            chdir(getenv("HOME"));
            execvp(shellPath.Raw(), argv);
        }

        i32 status;
    continue_waiting:
        if (waitpid(pid, &status, 0) == pid)
        {
            bool exited = WIFEXITED(status);
            if (!exited) goto continue_waiting;

            Info("Aurora: Child {} died with exit code {}", pid,
                 WEXITSTATUS(status));
        }
    }

    Trace("Aurora: Exiting...");
    return EXIT_SUCCESS;
}
