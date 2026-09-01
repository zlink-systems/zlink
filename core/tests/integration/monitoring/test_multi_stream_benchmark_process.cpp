/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
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

std::string sibling_binary_path (const char *argv0_, const char *name_)
{
    std::string path = argv0_ ? argv0_ : "";
    const std::string::size_type slash = path.find_last_of ('/');
    if (slash == std::string::npos)
        return std::string (name_);
    return path.substr (0, slash + 1) + name_;
}

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
    if (pipe (stdin_pipe) != 0 || pipe (stdout_pipe) != 0 || pipe (stderr_pipe) != 0) {
        return false;
    }

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

void write_stdin_line (process_capture_t *proc_, const char *line_)
{
    TEST_ASSERT_NOT_NULL (proc_);
    TEST_ASSERT_NOT_NULL (line_);
    TEST_ASSERT_TRUE (proc_->stdin_fd >= 0);
    const size_t size = std::strlen (line_);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (size),
                           static_cast<int> (write (proc_->stdin_fd, line_, size)));
}

bool stdout_contains_metric (process_capture_t *proc_, const char *metric_)
{
    if (!proc_ || !metric_)
        return false;

    std::lock_guard<std::mutex> lock (proc_->sync);
    for (size_t i = 0; i < proc_->stdout_lines.size (); ++i) {
        if (proc_->stdout_lines[i].find (metric_) != std::string::npos)
            return true;
    }
    return false;
}

void run_multi_stream_process_case (const char *recv_mode_, const char *transport_)
{
    process_capture_t server;
    process_capture_t client;

    const std::string server_path = sibling_binary_path (g_self_path, "comp_src_stream_server");
    const std::string client_path = sibling_binary_path (g_self_path, "perf_stream_client");

    std::vector<std::string> common_env;
    common_env.push_back ("PERF_MSG_SIZES=64");
    common_env.push_back ("PERF_DURATION_SECONDS=1");
    common_env.push_back ("PERF_WARMUP_SECONDS=1");
    common_env.push_back ("PERF_SETTLE_MS=500");
    common_env.push_back ("PERF_MULTI_CLIENTS=4");
    common_env.push_back ("PERF_MULTI_PRINT_AUTO_HWM_DETAIL=1");
    common_env.push_back (std::string ("PERF_RECV_MODE=") + recv_mode_);

    std::vector<std::string> server_args;
    server_args.push_back ("current");
    server_args.push_back (transport_);
    server_args.push_back ("64");
    TEST_ASSERT_TRUE (start_process (server_path, server_args, common_env, &server));

    std::string ready_line;
    TEST_ASSERT_TRUE (wait_for_stdout_prefix (&server, "READY,", 10000, &ready_line));
    std::string endpoint = ready_line.substr (strlen ("READY,"));
    if (!endpoint.empty () && endpoint[endpoint.size () - 1] == '\n')
        endpoint.erase (endpoint.size () - 1);

    std::vector<std::string> client_args;
    client_args.push_back ("--pattern");
    client_args.push_back ("MULTI_STREAM");
    client_args.push_back ("--transport");
    client_args.push_back (transport_);
    client_args.push_back ("--endpoint");
    client_args.push_back (endpoint);
    client_args.push_back ("--sizes");
    client_args.push_back ("64");
    client_args.push_back ("--duration");
    client_args.push_back ("1");
    client_args.push_back ("--warmup");
    client_args.push_back ("1");
    client_args.push_back ("--ccu");
    client_args.push_back ("4");
    client_args.push_back ("--runs");
    client_args.push_back ("1");
    client_args.push_back ("--print-perf-result");
    client_args.push_back ("1");
    client_args.push_back ("--start-gate");
    client_args.push_back ("1");

    TEST_ASSERT_TRUE (start_process (client_path, client_args, common_env, &client));

    std::string client_ready_line;
    TEST_ASSERT_TRUE (
      wait_for_stdout_prefix (&client, "CLIENT_READY,64", 10000, &client_ready_line));
    write_stdin_line (&server, "START,64\n");

    std::string server_start_ready_line;
    TEST_ASSERT_TRUE (wait_for_stdout_prefix (
      &server, "SERVER_START_READY,64", 10000, &server_start_ready_line));
    TEST_ASSERT_TRUE (stdout_contains_metric (&server, "label=server-connected"));
    write_stdin_line (&client, "START,64\n");

    int client_rc = INT_MIN;
    TEST_ASSERT_TRUE (wait_for_exit_code (&client, 30000, &client_rc));
    close_process_capture (&client);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, client_rc, client.stderr_text.c_str ());
    TEST_ASSERT_TRUE (stdout_contains_metric (
      &client,
      (std::string ("RESULT,current,MULTI_STREAM,") + transport_ + ",64,throughput").c_str ()));
    TEST_ASSERT_TRUE (stdout_contains_metric (
      &client,
      (std::string ("RESULT,current,MULTI_STREAM,") + transport_ + ",64,latency").c_str ()));

    write_stdin_line (&server, "STOP\n");

    int server_rc = INT_MIN;
    TEST_ASSERT_TRUE (wait_for_exit_code (&server, 15000, &server_rc));
    close_process_capture (&server);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, server_rc, server.stderr_text.c_str ());
}
} // namespace

void test_multi_stream_process_recv_smoke ()
{
    run_multi_stream_process_case ("recv", "tcp");
}

void test_multi_stream_process_recv_tls_smoke ()
{
    run_multi_stream_process_case ("recv", "tls");
}

void test_multi_stream_process_recv_wss_smoke ()
{
    run_multi_stream_process_case ("recv", "wss");
}

int main (int argc, char **argv)
{
    signal (SIGPIPE, SIG_IGN);
    g_self_path = argc > 0 ? argv[0] : NULL;
    UNITY_BEGIN ();
    RUN_TEST (test_multi_stream_process_recv_smoke);
    RUN_TEST (test_multi_stream_process_recv_tls_smoke);
    RUN_TEST (test_multi_stream_process_recv_wss_smoke);
    return UNITY_END ();
}

#else

int main ()
{
    TEST_IGNORE_MESSAGE ("POSIX-only process regression test");
    return 77;
}

#endif
