#pragma once
#include "structure.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace MyGame::E2E
{
// This owns only child-process IO. Framework execution is never synchronized here.
class Process
{
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> lines_;
    bool exited_ = false;
    int exit_code_ = -1;
    std::string read_error_;
    std::thread reader_;
#ifdef _WIN32
    HANDLE process_ = nullptr, input_ = nullptr, output_ = nullptr;
    static std::string quote (const std::string &argument)
    {
        std::string result = "\"";
        std::size_t slashes = 0;
        for (const char c : argument) {
            if (c == '\\') {
                ++slashes;
                continue;
            }
            result.append (slashes * (c == '"' ? 2 : 1), '\\');
            slashes = 0;
            if (c == '"')
                result += '\\';
            result += c;
        }
        result.append (slashes * 2, '\\');
        return result + '"';
    }
#else
    pid_t pid_ = -1;
    int input_ = -1, output_ = -1;
#endif
  public:
    Process (const std::vector<std::string> &arguments,
             const std::filesystem::path &out_log,
             const std::filesystem::path &err_log)
    {
        require (!arguments.empty (), "Empty child command");
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof (SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE child_in = nullptr, child_out = nullptr;
        if (!CreatePipe (&child_in, &input_, &security, 0))
            throw std::runtime_error ("CreatePipe failed");
        if (!CreatePipe (&output_, &child_out, &security, 0)) {
            CloseHandle (child_in);
            CloseHandle (input_);
            throw std::runtime_error ("CreatePipe failed");
        }
        SetHandleInformation (input_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation (output_, HANDLE_FLAG_INHERIT, 0);
        HANDLE err =
          CreateFileW (err_log.c_str (), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (err == INVALID_HANDLE_VALUE) {
            CloseHandle (child_in);
            CloseHandle (input_);
            CloseHandle (child_out);
            CloseHandle (output_);
            throw std::runtime_error ("Cannot open process stderr");
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof (startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_in;
        startup.hStdOutput = child_out;
        startup.hStdError = err;
        std::string command;
        for (const auto &argument : arguments) {
            if (!command.empty ())
                command += ' ';
            command += quote (argument);
        }
        PROCESS_INFORMATION info{};
        const bool started = CreateProcessA (nullptr, command.data (), nullptr, nullptr, TRUE,
                                             CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        CloseHandle (child_in);
        CloseHandle (child_out);
        CloseHandle (err);
        if (!started) {
            CloseHandle (input_);
            CloseHandle (output_);
            input_ = output_ = nullptr;
            throw std::runtime_error ("CreateProcess failed: " + std::to_string (GetLastError ()));
        }
        process_ = info.hProcess;
        CloseHandle (info.hThread);
#else
        int in[2], out[2];
        if (pipe2 (in, O_CLOEXEC) != 0)
            throw std::runtime_error ("pipe failed");
        if (pipe2 (out, O_CLOEXEC) != 0) {
            close (in[0]);
            close (in[1]);
            throw std::runtime_error ("pipe failed");
        }
        const int err = open (err_log.c_str (), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        if (err < 0) {
            close (in[0]);
            close (in[1]);
            close (out[0]);
            close (out[1]);
            throw std::runtime_error ("Cannot open process stderr");
        }
        std::vector<char *> argv;
        for (const auto &argument : arguments)
            argv.push_back (const_cast<char *> (argument.c_str ()));
        argv.push_back (nullptr);
        pid_ = fork ();
        if (pid_ == 0) {
            dup2 (in[0], STDIN_FILENO);
            dup2 (out[1], STDOUT_FILENO);
            dup2 (err, STDERR_FILENO);
            close (in[0]);
            close (in[1]);
            close (out[0]);
            close (out[1]);
            close (err);
            execvp (argv[0], argv.data ());
            _exit (127);
        }
        close (in[0]);
        close (out[1]);
        close (err);
        if (pid_ < 0) {
            close (in[1]);
            close (out[0]);
            throw std::runtime_error ("fork failed");
        }
        input_ = in[1];
        output_ = out[0];
#endif
        reader_ = std::thread ([this, out_log] {
            std::ofstream log (out_log);
            std::string pending;
            char buffer[4096];
            for (;;) {
#ifdef _WIN32
                DWORD count = 0;
                if (!ReadFile (output_, buffer, sizeof (buffer), &count, nullptr) || count == 0)
                    break;
#else
                const auto count = read (output_, buffer, sizeof (buffer));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
#endif
                log.write (buffer, count);
                log.flush ();
                pending.append (buffer, count);
                std::size_t end;
                while ((end = pending.find ('\n')) != std::string::npos) {
                    auto line = pending.substr (0, end);
                    pending.erase (0, end + 1);
                    if (!line.empty () && line.back () == '\r')
                        line.pop_back ();
                    {
                        std::lock_guard lock (mutex_);
                        lines_.push_back (std::move (line));
                    }
                    changed_.notify_all ();
                }
            }
            int code;
#ifdef _WIN32
            WaitForSingleObject (process_, INFINITE);
            DWORD value;
            GetExitCodeProcess (process_, &value);
            code = static_cast<int> (value);
#else
            int status = 0;
            while (waitpid (pid_, &status, 0) < 0)
                if (errno != EINTR)
                    break;
            code = WIFEXITED (status) ? WEXITSTATUS (status) : 128 + WTERMSIG (status);
#endif
            {
                std::lock_guard lock (mutex_);
                if (!pending.empty ())
                    lines_.push_back (pending);
                if (!log)
                    read_error_ = "Process output log failed";
                exit_code_ = code;
                exited_ = true;
            }
            changed_.notify_all ();
        });
    }
    Process (const Process &) = delete;
    Process &operator= (const Process &) = delete;
    ~Process ()
    {
        if (reader_.joinable ()) {
            bool exited;
            {
                std::lock_guard lock (mutex_);
                exited = exited_;
            }
            if (!exited)
                terminate ();
            reader_.join ();
        }
#ifdef _WIN32
        if (input_)
            CloseHandle (input_);
        if (output_)
            CloseHandle (output_);
        if (process_)
            CloseHandle (process_);
#else
        if (input_ >= 0)
            close (input_);
        if (output_ >= 0)
            close (output_);
#endif
    }
    void terminate () noexcept
    {
#ifdef _WIN32
        TerminateProcess (process_, 1);
#else
        kill (pid_, SIGKILL);
#endif
    }
    void command (const std::string &text)
    {
        const auto line = text + '\n';
#ifdef _WIN32
        DWORD count;
        require (
          WriteFile (input_, line.data (), static_cast<DWORD> (line.size ()), &count, nullptr)
            && count == line.size (),
          "Child stdin closed");
#else
        std::size_t offset = 0;
        while (offset < line.size ()) {
            const auto count = write (input_, line.data () + offset, line.size () - offset);
            if (count < 0 && errno == EINTR)
                continue;
            require (count > 0, "Child stdin closed");
            offset += static_cast<std::size_t> (count);
        }
#endif
    }
    std::string wait_line (const std::string &prefix,
                           std::chrono::milliseconds timeout = Limits::startup)
    {
        std::unique_lock lock (mutex_);
        const auto match = [&] () {
            return std::find_if (lines_.begin (), lines_.end (),
                                 [&] (const auto &line) { return line.starts_with (prefix); });
        };
        require (
          changed_.wait_for (lock, timeout, [&] { return match () != lines_.end () || exited_; }),
          "Process output deadline: " + prefix);
        const auto found = match ();
        require (found != lines_.end (),
                 "Process exited before " + prefix + " (exit " + std::to_string (exit_code_) + ")");
        return *found;
    }
    int wait_exit (std::chrono::milliseconds timeout = Limits::shutdown)
    {
        std::unique_lock lock (mutex_);
        require (changed_.wait_for (lock, timeout, [&] { return exited_; }),
                 "Process exit deadline");
        require (read_error_.empty (), read_error_);
        return exit_code_;
    }
    std::string output ()
    {
        std::lock_guard lock (mutex_);
        std::ostringstream result;
        for (const auto &line : lines_)
            result << line << '\n';
        return result.str ();
    }
};
}
