// SPDX-License-Identifier: Apache-2.0
#include "platform/file_dialog.h"

#include "platform/platform.h"

#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace noise::platform {

namespace {

std::string picker_program() {
    const auto configured = env("NOISE_PICKER_PROGRAM");
    if (configured && !configured->empty()) {
        return *configured;
    }
    return "zenity";
}

std::string read_all(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        if (got > 0) {
            out.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return out;
    }
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            if (!current.empty()) {
                lines.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

}  // namespace

PickResult pick_path(const PickRequest& request) {
    PickResult result;
    const std::string program = picker_program();

    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        result.error = "could not open a pipe to " + program + ": " + std::strerror(errno);
        return result;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        result.error = "could not start " + program + ": " + std::strerror(errno);
        return result;
    }
    if (child == 0) {
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        const std::vector<std::string> argv = picker_argv(program, request);
        std::vector<char*> words;
        words.reserve(argv.size() + 1);
        for (const std::string& word : argv) {
            words.push_back(const_cast<char*>(word.c_str()));
        }
        words.push_back(nullptr);
        ::execvp(program.c_str(), words.data());
        ::_exit(127);  // execvp only returns on failure, and 127 is the shell's word for it
    }

    ::close(pipefd[1]);
    const std::string printed = read_all(pipefd[0]);
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status)) {
        result.error = program + " was killed by a signal before answering";
        return result;
    }
    const int code = WEXITSTATUS(status);
    const std::vector<std::string> paths = split_lines(printed);
    if (code == 0) {
        if (paths.empty()) {
            result.error = program + " finished without naming a file";
            return result;
        }
        result.paths = paths;
        return result;
    }
    if (code == 1) {
        result.cancelled = true;
        return result;
    }
    if (code == 127) {
        result.error = program + " is not installed; install it or point $NOISE_PICKER_PROGRAM at a "
                               "picker that is (open: '" + request.title + "')";
        return result;
    }
    result.error = program + " failed with exit code " + std::to_string(code) + " (open: '" +
                   request.title + "')";
    return result;
}

}  // namespace noise::platform
