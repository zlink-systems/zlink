/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#ifndef _WIN32

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
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

bool should_run_pubsub_process_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}

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
        if (stdout_stream_) {
            proc_->stdout_lines.push_back (std::string (buffer));
        } else {
            proc_->stderr_text.append (buffer);
        }
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

void run_multi_pubsub_process_case (const char *self_path_,
                                    const char *recv_mode_,
                                    const char *msg_size_ = "64",
                                    int clients_ = 1,
                                    int connect_ready_timeout_ms_ = 15000)
{
    process_capture_t server;
    process_capture_t client;

    const std::string server_path = sibling_binary_path (self_path_, "comp_src_pubsub_server");
    const std::string client_path = sibling_binary_path (self_path_, "comp_src_pubsub_client");

    std::vector<std::string> server_env;
    server_env.push_back (std::string ("PERF_MSG_SIZES=") + msg_size_);
    server_env.push_back ("PERF_DURATION_SECONDS=1");
    server_env.push_back ("PERF_WARMUP_SECONDS=0");
    server_env.push_back ("PERF_SETTLE_MS=500");
    {
        char buf[32];
        snprintf (buf, sizeof (buf), "PERF_CLIENTS=%d", clients_);
        server_env.push_back (buf);
    }
    {
        char buf[64];
        snprintf (buf, sizeof (buf), "PERF_CONNECT_READY_TIMEOUT_MS=%d", connect_ready_timeout_ms_);
        server_env.push_back (buf);
    }
    std::vector<std::string> server_args;
    server_args.push_back ("zlink");
    server_args.push_back ("tcp");
    TEST_ASSERT_TRUE (start_process (server_path, server_args, server_env, &server));

    std::string ready_line;
    TEST_ASSERT_TRUE (wait_for_stdout_prefix (&server, "READY,", 10000, &ready_line));
    std::string endpoint = ready_line.substr (strlen ("READY,"));
    if (!endpoint.empty () && endpoint[endpoint.size () - 1] == '\n')
        endpoint.erase (endpoint.size () - 1);

    std::vector<std::string> client_args;
    client_args.push_back ("zlink");
    client_args.push_back ("tcp");
    client_args.push_back (msg_size_);
    client_args.push_back ("--endpoint");
    client_args.push_back (endpoint);

    std::vector<std::string> client_env = server_env;
    client_env.push_back (std::string ("PERF_RECV_MODE=") + recv_mode_);
    TEST_ASSERT_TRUE (start_process (client_path, client_args, client_env, &client));

    TEST_ASSERT_TRUE (wait_for_stdout_prefix (&client, "CLIENT_READY,", 10000, NULL));
    const std::string start_line = std::string ("START,") + msg_size_ + "\n";
    write_stdin_line (&server, start_line.c_str ());
    write_stdin_line (&client, start_line.c_str ());

    int client_rc = INT_MIN;
    TEST_ASSERT_TRUE (wait_for_exit_code (&client, 20000, &client_rc));
    close_process_capture (&client);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, client_rc, client.stderr_text.c_str ());

    std::lock_guard<std::mutex> client_lock (client.sync);
    bool saw_throughput = false;
    for (size_t i = 0; i < client.stdout_lines.size (); ++i) {
        if (client.stdout_lines[i].find ("throughput") != std::string::npos) {
            saw_throughput = true;
            break;
        }
    }
    TEST_ASSERT_TRUE (saw_throughput);

    write_stdin_line (&server, "STOP\n");
    int server_rc = INT_MIN;
    TEST_ASSERT_TRUE (wait_for_exit_code (&server, 20000, &server_rc));
    close_process_capture (&server);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, server_rc, server.stderr_text.c_str ());
}

void run_multi_pubsub_callback_reject_case (const char *self_path_)
{
    process_capture_t client;
    const std::string client_path = sibling_binary_path (self_path_, "comp_src_pubsub_client");

    std::vector<std::string> client_args;
    client_args.push_back ("zlink");
    client_args.push_back ("tcp");
    client_args.push_back ("64");
    client_args.push_back ("--endpoint");
    client_args.push_back ("tcp://127.0.0.1:1");

    std::vector<std::string> client_env;
    client_env.push_back ("PERF_RECV_MODE=callback");
    client_env.push_back ("PERF_MULTI_PATTERN=MULTI_PUBSUB");

    TEST_ASSERT_TRUE (start_process (client_path, client_args, client_env, &client));

    int client_rc = INT_MIN;
    TEST_ASSERT_TRUE (wait_for_exit_code (&client, 15000, &client_rc));
    close_process_capture (&client);
    TEST_ASSERT_NOT_EQUAL (0, client_rc);
    TEST_ASSERT_TRUE (client.stderr_text.find ("policy violation") != std::string::npos);
}
}

void test_multi_pubsub_process_recv_preserves_settle_and_active_window ()
{
    run_multi_pubsub_process_case (g_self_path, "recv");
}

void test_multi_pubsub_process_recv_large_size_completes ()
{
    run_multi_pubsub_process_case (g_self_path, "recv", "262144");
}

void test_multi_pubsub_process_recv_1000_clients_tcp_ready_count ()
{
    run_multi_pubsub_process_case (g_self_path, "recv", "64", 1000, 20000);
}

void test_multi_pubsub_process_callback_is_rejected ()
{
    run_multi_pubsub_callback_reject_case (g_self_path);
}

int main (int argc, char **argv)
{
    signal (SIGPIPE, SIG_IGN);
    g_self_path = argc > 0 ? argv[0] : NULL;
    UNITY_BEGIN ();
#define RUN_PUBSUB_PROCESS_TEST(name)                                                              \
    do {                                                                                           \
        if (should_run_pubsub_process_test (#name))                                                \
            RUN_TEST (name);                                                                       \
    } while (0)
    RUN_PUBSUB_PROCESS_TEST (test_multi_pubsub_process_recv_preserves_settle_and_active_window);
    RUN_PUBSUB_PROCESS_TEST (test_multi_pubsub_process_recv_large_size_completes);
    RUN_PUBSUB_PROCESS_TEST (test_multi_pubsub_process_recv_1000_clients_tcp_ready_count);
    RUN_PUBSUB_PROCESS_TEST (test_multi_pubsub_process_callback_is_rejected);
#undef RUN_PUBSUB_PROCESS_TEST
    return UNITY_END ();
}

#else

int main ()
{
    TEST_IGNORE_MESSAGE ("POSIX-only process regression test");
    return 77;
}

#endif
