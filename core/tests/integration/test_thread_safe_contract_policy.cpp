/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <stdio.h>
#include <string>

#ifndef TEST_REPO_ROOT
#define TEST_REPO_ROOT ""
#endif

#ifndef TEST_ZLINK_HEADER_PATH
#define TEST_ZLINK_HEADER_PATH ""
#endif

namespace
{
std::string read_text_file (const char *path_)
{
    TEST_ASSERT_NOT_NULL (path_);
    TEST_ASSERT_TRUE (path_[0] != '\0');

    FILE *fp = fopen (path_, "rb");
    if (!fp) {
        char msg[512];
        snprintf (msg, sizeof (msg), "failed to open text file: %s", path_);
        TEST_FAIL_MESSAGE (msg);
    }

    std::string content;
    char buf[4096];
    while (true) {
        const size_t n = fread (buf, 1, sizeof (buf), fp);
        if (n > 0)
            content.append (buf, n);
        if (n < sizeof (buf))
            break;
    }

    const int file_error = ferror (fp);
    fclose (fp);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, file_error, "failed while reading public header");
    return content;
}

std::string dirname_of (const std::string &path_)
{
    const std::string::size_type slash = path_.find_last_of ("/\\");
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return path_.substr (0, 1);
    return path_.substr (0, slash);
}

std::string
resolve_public_header_text (const std::string &path_, const std::string &include_root_, int depth_)
{
    TEST_ASSERT_TRUE_MESSAGE (depth_ >= 0, "public header include depth exceeded");

    const std::string content = read_text_file (path_.c_str ());
    std::string resolved = content;
    const std::string base_dir = dirname_of (path_);

    std::string::size_type pos = 0;
    while (true) {
        pos = content.find ("#include ", pos);
        if (pos == std::string::npos)
            break;

        const std::string::size_type delimiter = pos + 9;
        if (delimiter >= content.size ())
            break;

        const char open = content[delimiter];
        const char close = open == '"' ? '"' : open == '<' ? '>' : '\0';
        if (close == '\0') {
            ++pos;
            continue;
        }

        const std::string::size_type start = delimiter + 1;
        const std::string::size_type end = content.find (close, start);
        if (end == std::string::npos)
            break;

        const std::string include_name = content.substr (start, end - start);
        if (open == '<' && include_name.find ("zlink/") != 0) {
            pos = end + 1;
            continue;
        }

        const std::string include_path = open == '<' && include_name.find ("zlink/") == 0
                                           ? include_root_ + "/" + include_name
                                           : base_dir + "/" + include_name;
        resolved.append ("\n");
        resolved.append (resolve_public_header_text (include_path, include_root_, depth_ - 1));
        pos = end + 1;
    }

    return resolved;
}

std::string read_public_header_contract_text ()
{
    const std::string include_root = dirname_of (TEST_ZLINK_HEADER_PATH);
    return resolve_public_header_text (TEST_ZLINK_HEADER_PATH, include_root, 8);
}

void assert_text_absent (const std::string &text_, const char *needle_)
{
    TEST_ASSERT_NOT_NULL (needle_);

    char msg[192];
    snprintf (msg, sizeof (msg), "public header must not expose forbidden contract token: %s",
              needle_);
    TEST_ASSERT_TRUE_MESSAGE (text_.find (needle_) == std::string::npos, msg);
}

void assert_text_present (const std::string &text_, const char *needle_)
{
    TEST_ASSERT_NOT_NULL (needle_);

    char msg[192];
    snprintf (msg, sizeof (msg), "required contract token missing from checked text: %s", needle_);
    TEST_ASSERT_TRUE_MESSAGE (text_.find (needle_) != std::string::npos, msg);
}

void test_public_header_omits_selectable_thread_mode_contract ()
{
    const std::string header = read_public_header_contract_text ();
    TEST_ASSERT_FALSE (header.empty ());

    assert_text_absent (header, "zlink_thread_mode_t");
    assert_text_absent (header, "thread_mode");
    assert_text_absent (header, "_set_thread_mode");
    assert_text_absent (header, "set_recv_handler");
    assert_text_absent (header, "_set_handler");
    assert_text_absent (header, "_use_lock");
    assert_text_absent (header, "same-handle operational APIs");
}

void test_public_header_retains_send_ready_and_monitor_surface ()
{
    const std::string header = read_public_header_contract_text ();
    TEST_ASSERT_FALSE (header.empty ());

    assert_text_present (header, "zlink_send_ready_handler");
    assert_text_present (header, "zlink_socket_monitor_open");
    assert_text_present (header, "zlink_socket_monitor_handler");
    assert_text_absent (header, "zlink_service_monitor_open");
    assert_text_absent (header, "zlink_service_monitor_handler");
    assert_text_absent (header, "zlink_service_monitor_recv");
    assert_text_absent (header, "zlink_service_monitor_ignore_handler");
    assert_text_present (header, "zlink_monitor_close");
}

void test_docs_reflect_tiered_thread_safe_contract ()
{
    TEST_ASSERT_TRUE (TEST_REPO_ROOT[0] != '\0');

    const std::string socket_doc =
      read_text_file (
        (std::string (TEST_REPO_ROOT) + "/core/doc/spec/core/socket/README.en.md").c_str ());
    const std::string socket_doc_ko = read_text_file (
      (std::string (TEST_REPO_ROOT) + "/core/doc/spec/core/socket/README.ko.md").c_str ());
    const std::string threading_doc = read_text_file (
      (std::string (TEST_REPO_ROOT) + "/core/doc/internals/threading-model.en.md").c_str ());
    const std::string threading_doc_ko = read_text_file (
      (std::string (TEST_REPO_ROOT) + "/core/doc/internals/threading-model.ko.md").c_str ());

    assert_text_present (socket_doc, "tiered contract");
    assert_text_present (threading_doc, "`send`/`publish`/`send_rid`");
    assert_text_present (socket_doc, "can be called concurrently from multiple threads");
    assert_text_present (socket_doc, "fails with `errno=ESHUTDOWN`");
    assert_text_present (threading_doc, "control paths serialize for correctness");
    assert_text_present (threading_doc_ko, "여러 스레드에서 동시 사용 가능");

    assert_text_absent (socket_doc, "same-handle operational APIs");
    assert_text_absent (socket_doc_ko, "모든 operational API 동일 강도 thread-safe");
    assert_text_absent (socket_doc, "control-plane = 초기화 단계 전용");
}
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_public_header_omits_selectable_thread_mode_contract);
    RUN_TEST (test_public_header_retains_send_ready_and_monitor_surface);
    RUN_TEST (test_docs_reflect_tiered_thread_safe_contract);
    return UNITY_END ();
}
