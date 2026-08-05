/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace
{
const char *g_self_path = NULL;

struct process_capture_t
{
    process_capture_t () :
        pid (-1),
        stdin_fd (-1),
        stdout_file (NULL),
        stderr_file (NULL),
        stdout_done (false),
        stderr_done (false)
    {
    }

    pid_t pid;
    int stdin_fd;
    FILE *stdout_file;
    FILE *stderr_file;
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::mutex sync;
    std::condition_variable cv;
    std::vector<std::string> stdout_lines;
    std::string stderr_text;
    bool stdout_done;
    bool stderr_done;
};

void close_process_capture (process_capture_t *proc_)
{
    if (!proc_)
        return;

    if (proc_->stdin_fd >= 0) {
        close (proc_->stdin_fd);
        proc_->stdin_fd = -1;
    }
    if (proc_->stdout_thread.joinable ())
        proc_->stdout_thread.join ();
    if (proc_->stderr_thread.joinable ())
        proc_->stderr_thread.join ();
    if (proc_->stdout_file) {
        fclose (proc_->stdout_file);
        proc_->stdout_file = NULL;
    }
    if (proc_->stderr_file) {
        fclose (proc_->stderr_file);
        proc_->stderr_file = NULL;
    }
}

void cleanup_child (process_capture_t *proc_)
{
    if (!proc_ || proc_->pid <= 0)
        return;

    int status = 0;
    const pid_t waited = waitpid (proc_->pid, &status, WNOHANG);
    if (waited == 0) {
        kill (proc_->pid, SIGKILL);
        waitpid (proc_->pid, &status, 0);
    }
    proc_->pid = -1;
    close_process_capture (proc_);
}

void reader_loop (FILE *stream_, process_capture_t *proc_, bool stdout_stream_)
{
    char buffer[512];
    while (stream_ && fgets (buffer, sizeof (buffer), stream_)) {
        std::lock_guard<std::mutex> lock (proc_->sync);
        if (stdout_stream_)
            proc_->stdout_lines.push_back (std::string (buffer));
        else
            proc_->stderr_text.append (buffer);
        proc_->cv.notify_all ();
    }

    std::lock_guard<std::mutex> lock (proc_->sync);
    if (stdout_stream_)
        proc_->stdout_done = true;
    else
        proc_->stderr_done = true;
    proc_->cv.notify_all ();
}

std::string sibling_binary_path (const char *argv0_, const char *name_)
{
    std::string path = argv0_ ? argv0_ : "";
    const std::string::size_type slash = path.find_last_of ('/');
    if (slash == std::string::npos)
        return std::string (name_);
    return path.substr (0, slash + 1) + name_;
}

bool start_process (const std::string &path_,
                    const std::vector<std::string> &args_,
                    const std::vector<std::string> &env_overrides_,
                    process_capture_t *out_)
{
    if (!out_)
        return false;

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe (stdin_pipe) != 0 || pipe (stdout_pipe) != 0 || pipe (stderr_pipe) != 0)
        return false;

    const pid_t pid = fork ();
    if (pid < 0) {
        close (stdin_pipe[0]);
        close (stdin_pipe[1]);
        close (stdout_pipe[0]);
        close (stdout_pipe[1]);
        close (stderr_pipe[0]);
        close (stderr_pipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2 (stdin_pipe[0], STDIN_FILENO);
        dup2 (stdout_pipe[1], STDOUT_FILENO);
        dup2 (stderr_pipe[1], STDERR_FILENO);

        close (stdin_pipe[0]);
        close (stdin_pipe[1]);
        close (stdout_pipe[0]);
        close (stdout_pipe[1]);
        close (stderr_pipe[0]);
        close (stderr_pipe[1]);

        std::vector<std::string> env_storage;
        for (char **it = environ; it && *it; ++it)
            env_storage.push_back (*it);
        env_storage.insert (env_storage.end (), env_overrides_.begin (), env_overrides_.end ());

        std::vector<char *> envp;
        for (size_t i = 0; i < env_storage.size (); ++i)
            envp.push_back (const_cast<char *> (env_storage[i].c_str ()));
        envp.push_back (NULL);

        std::vector<char *> argv;
        argv.push_back (const_cast<char *> (path_.c_str ()));
        for (size_t i = 0; i < args_.size (); ++i)
            argv.push_back (const_cast<char *> (args_[i].c_str ()));
        argv.push_back (NULL);

        execve (path_.c_str (), &argv[0], &envp[0]);
        _exit (127);
    }

    close (stdin_pipe[0]);
    close (stdout_pipe[1]);
    close (stderr_pipe[1]);

    out_->pid = pid;
    out_->stdin_fd = stdin_pipe[1];
    out_->stdout_file = fdopen (stdout_pipe[0], "r");
    out_->stderr_file = fdopen (stderr_pipe[0], "r");
    if (!out_->stdout_file || !out_->stderr_file) {
        cleanup_child (out_);
        return false;
    }

    out_->stdout_thread = std::thread (&reader_loop, out_->stdout_file, out_, true);
    out_->stderr_thread = std::thread (&reader_loop, out_->stderr_file, out_, false);
    return true;
}

bool wait_for_stdout_prefix (process_capture_t *proc_,
                             const char *prefix_,
                             int timeout_ms,
                             std::string *line_out_)
{
    if (!proc_ || !prefix_)
        return false;

    std::unique_lock<std::mutex> lock (proc_->sync);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms > 0 ? timeout_ms : 1);

    while (std::chrono::steady_clock::now () < deadline) {
        for (size_t i = 0; i < proc_->stdout_lines.size (); ++i) {
            if (proc_->stdout_lines[i].compare (0, std::strlen (prefix_), prefix_) == 0) {
                if (line_out_)
                    *line_out_ = proc_->stdout_lines[i];
                return true;
            }
        }
        if (proc_->stdout_done)
            return false;
        proc_->cv.wait_until (lock, deadline);
    }

    return false;
}

bool wait_for_exit_code (process_capture_t *proc_, int timeout_ms, int *rc_out_)
{
    if (!proc_ || proc_->pid <= 0)
        return false;

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (timeout_ms > 0 ? timeout_ms : 1);

    int status = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        const pid_t waited = waitpid (proc_->pid, &status, WNOHANG);
        if (waited == proc_->pid) {
            proc_->pid = -1;
            if (rc_out_) {
                if (WIFEXITED (status))
                    *rc_out_ = WEXITSTATUS (status);
                else if (WIFSIGNALED (status))
                    *rc_out_ = -WTERMSIG (status);
                else
                    *rc_out_ = -1;
            }
            return true;
        }
        msleep (10);
    }

    return false;
}

bool write_stdin_line (process_capture_t *proc_, const char *line_)
{
    if (!proc_ || !line_ || proc_->stdin_fd < 0) {
        errno = EINVAL;
        return false;
    }
    const size_t size = std::strlen (line_);
    const ssize_t written = write (proc_->stdin_fd, line_, size);
    return written == static_cast<ssize_t> (size);
}

std::string capture_debug_text (process_capture_t *proc_)
{
    if (!proc_)
        return std::string ();

    std::lock_guard<std::mutex> lock (proc_->sync);
    std::string text;
    if (!proc_->stderr_text.empty ()) {
        text += "stderr:\n";
        text += proc_->stderr_text;
    }
    if (!proc_->stdout_lines.empty ()) {
        if (!text.empty ())
            text += '\n';
        text += "stdout:\n";
        for (size_t i = 0; i < proc_->stdout_lines.size (); ++i)
            text += proc_->stdout_lines[i];
    }
    return text;
}

std::string capture_case_debug_text (process_capture_t *server_, process_capture_t *client_)
{
    std::string text;
    const std::string server_text = capture_debug_text (server_);
    const std::string client_text = capture_debug_text (client_);

    if (!server_text.empty ()) {
        text += "server:\n";
        text += server_text;
    }
    if (!client_text.empty ()) {
        if (!text.empty ())
            text += '\n';
        text += "client:\n";
        text += client_text;
    }

    return text;
}

void destroy_process_capture (process_capture_t *proc_)
{
    if (!proc_)
        return;
    cleanup_child (proc_);
    delete proc_;
}

void run_multi_dealer_dealer_tls_sequence_case ()
{
    process_capture_t *server = new process_capture_t ();
    process_capture_t *client = new process_capture_t ();

    const std::string server_path =
      sibling_binary_path (g_self_path, "comp_src_dealer_dealer_server");
    const std::string client_path =
      sibling_binary_path (g_self_path, "comp_src_dealer_dealer_client");

    std::vector<std::string> common_env;
    common_env.push_back ("PERF_MSG_SIZES=64,256,1024,65536,131072,262144");
    common_env.push_back ("PERF_DURATION_SECONDS=1");
    common_env.push_back ("PERF_WARMUP_SECONDS=1");
    common_env.push_back ("PERF_SETTLE_MS=500");
    common_env.push_back ("PERF_CLIENTS=8");
    common_env.push_back ("PERF_CONNECT_READY_TIMEOUT_MS=10000");
    common_env.push_back ("PERF_RECV_MODE=recv");

    std::vector<std::string> server_args;
    server_args.push_back ("zlink");
    server_args.push_back ("tls");
    TEST_ASSERT_TRUE (start_process (server_path, server_args, common_env, server));

    std::string ready_line;
    TEST_ASSERT_TRUE_MESSAGE (wait_for_stdout_prefix (server, "READY,", 15000, &ready_line),
                              capture_case_debug_text (server, client).c_str ());
    std::string endpoint = ready_line.substr (strlen ("READY,"));
    if (!endpoint.empty () && endpoint[endpoint.size () - 1] == '\n')
        endpoint.erase (endpoint.size () - 1);

    std::vector<std::string> client_args;
    client_args.push_back ("zlink");
    client_args.push_back ("tls");
    client_args.push_back ("64");
    client_args.push_back ("--endpoint");
    client_args.push_back (endpoint);
    TEST_ASSERT_TRUE (start_process (client_path, client_args, common_env, client));

    const char *sizes[] = {"64", "256", "1024", "65536", "131072", "262144"};
    for (size_t i = 0; i < sizeof (sizes) / sizeof (*sizes); ++i) {
        const std::string ready_line = std::string ("CLIENT_READY,") + sizes[i];
        TEST_ASSERT_TRUE_MESSAGE (wait_for_stdout_prefix (client, ready_line.c_str (), 30000, NULL),
                                  capture_case_debug_text (server, client).c_str ());
        TEST_ASSERT_TRUE_MESSAGE (
          write_stdin_line (server, (std::string ("START,") + sizes[i] + "\n").c_str ()),
          capture_case_debug_text (server, client).c_str ());
        TEST_ASSERT_TRUE_MESSAGE (
          write_stdin_line (client, (std::string ("START,") + sizes[i] + "\n").c_str ()),
          capture_case_debug_text (server, client).c_str ());
        const std::string done_line = std::string ("CLIENT_DONE,") + sizes[i];
        TEST_ASSERT_TRUE_MESSAGE (wait_for_stdout_prefix (client, done_line.c_str (), 120000, NULL),
                                  capture_case_debug_text (server, client).c_str ());
        const std::string server_done_prefix =
          std::string ("RESULT,zlink,MULTI_DEALER_DEALER,tls,") + sizes[i] + ",throughput,";
        TEST_ASSERT_TRUE_MESSAGE (
          wait_for_stdout_prefix (server, server_done_prefix.c_str (), 120000, NULL),
          capture_case_debug_text (server, client).c_str ());
    }
    TEST_ASSERT_TRUE_MESSAGE (write_stdin_line (server, "STOP\n"),
                              capture_case_debug_text (server, client).c_str ());

    int client_rc = INT_MIN;
    TEST_ASSERT_TRUE_MESSAGE (wait_for_exit_code (client, 120000, &client_rc),
                              capture_case_debug_text (server, client).c_str ());
    close_process_capture (client);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, client_rc, capture_case_debug_text (server, client).c_str ());

    int server_rc = INT_MIN;
    TEST_ASSERT_TRUE_MESSAGE (wait_for_exit_code (server, 120000, &server_rc),
                              capture_case_debug_text (server, client).c_str ());
    close_process_capture (server);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, server_rc, capture_case_debug_text (server, client).c_str ());

    destroy_process_capture (client);
    destroy_process_capture (server);
}
} // namespace

void setUp ()
{
}

void tearDown ()
{
}

void test_multi_dealer_dealer_process_tls_large_sequence ()
{
    run_multi_dealer_dealer_tls_sequence_case ();
}

int main (int argc, char **argv)
{
    g_self_path = argc > 0 ? argv[0] : NULL;
    UNITY_BEGIN ();
    RUN_TEST (test_multi_dealer_dealer_process_tls_large_sequence);
    return UNITY_END ();
}

#else

int main ()
{
    TEST_IGNORE_MESSAGE ("POSIX-only process regression test");
    return 77;
}

#endif
