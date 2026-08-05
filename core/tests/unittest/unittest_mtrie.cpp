/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#if defined(min)
#undef min
#endif

#include <utils/generic_mtrie_impl.hpp>

#include <string>

#include <unity.h>

void setUp ()
{
}
void tearDown ()
{
}

int getlen (const zlink::generic_mtrie_t<int>::prefix_t &data_)
{
    return static_cast<int> (strlen (reinterpret_cast<const char *> (data_)));
}

void test_create ()
{
    zlink::generic_mtrie_t<int> mtrie;
}

void mtrie_count (int *pipe_, int *count_)
{
    LIBZLINK_UNUSED (pipe_);
    ++*count_;
}

void test_check_empty_match_nonempty_data ()
{
    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (0, count);
}

void test_check_empty_match_empty_data ()
{
    zlink::generic_mtrie_t<int> mtrie;

    int count = 0;
    mtrie.match (NULL, 0, mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (0, count);
}

void test_add_single_entry_match_exact ()
{
    int pipe;

    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    bool res = mtrie.add (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_TRUE (res);

    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (1, count);
}

void test_add_single_entry_twice_match_exact ()
{
    int pipe;

    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    bool res = mtrie.add (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_TRUE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    res = mtrie.add (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_FALSE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (1, count);
}

void test_add_two_entries_with_same_name_match_exact ()
{
    int pipe_1, pipe_2;

    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    bool res = mtrie.add (test_name, getlen (test_name), &pipe_1);
    TEST_ASSERT_TRUE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    res = mtrie.add (test_name, getlen (test_name), &pipe_2);
    TEST_ASSERT_FALSE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (2, count);
}

void test_add_two_entries_match_prefix_and_exact ()
{
    int pipe_1, pipe_2;

    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name_prefix =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");
    const zlink::generic_mtrie_t<int>::prefix_t test_name_full =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foobar");

    bool res = mtrie.add (test_name_prefix, getlen (test_name_prefix), &pipe_1);
    TEST_ASSERT_TRUE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    res = mtrie.add (test_name_full, getlen (test_name_full), &pipe_2);
    TEST_ASSERT_TRUE (res);
    TEST_ASSERT_EQUAL_INT (2, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name_full, getlen (test_name_full), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (2, count);
}

void test_add_rm_single_entry_match_exact ()
{
    int pipe;
    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    mtrie.add (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());
    zlink::generic_mtrie_t<int>::rm_result res = mtrie.rm (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::last_value_removed, res);
    TEST_ASSERT_EQUAL_INT (0, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (0, count);
}

void test_rm_nonexistent_0_size_empty ()
{
    int pipe;
    zlink::generic_mtrie_t<int> mtrie;

    zlink::generic_mtrie_t<int>::rm_result res = mtrie.rm (0, 0, &pipe);
    TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::not_found, res);
    TEST_ASSERT_EQUAL_INT (0, mtrie.num_prefixes ());
}

void test_rm_nonexistent_empty ()
{
    int pipe;
    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t test_name =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> ("foo");

    zlink::generic_mtrie_t<int>::rm_result res = mtrie.rm (test_name, getlen (test_name), &pipe);
    TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::not_found, res);
    TEST_ASSERT_EQUAL_INT (0, mtrie.num_prefixes ());

    int count = 0;
    mtrie.match (test_name, getlen (test_name), mtrie_count, &count);
    TEST_ASSERT_EQUAL_INT (0, count);
}

void test_add_and_rm_other (const char *add_name_, const char *rm_name_)
{
    int addpipe, rmpipe;
    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t add_name_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (add_name_);
    const zlink::generic_mtrie_t<int>::prefix_t rm_name_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (rm_name_);

    mtrie.add (add_name_data, getlen (add_name_data), &addpipe);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    zlink::generic_mtrie_t<int>::rm_result res =
      mtrie.rm (rm_name_data, getlen (rm_name_data), &rmpipe);
    TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::not_found, res);
    TEST_ASSERT_EQUAL_INT (1, mtrie.num_prefixes ());

    {
        int count = 0;
        mtrie.match (add_name_data, getlen (add_name_data), mtrie_count, &count);
        TEST_ASSERT_EQUAL_INT (1, count);
    }

    if (strncmp (add_name_, rm_name_, std::min (strlen (add_name_), strlen (rm_name_) + 1)) != 0) {
        int count = 0;
        mtrie.match (rm_name_data, getlen (rm_name_data), mtrie_count, &count);
        TEST_ASSERT_EQUAL_INT (0, count);
    }
}

void test_rm_nonexistent_nonempty_samename ()
{
    test_add_and_rm_other ("foo", "foo");
}

void test_rm_nonexistent_nonempty_differentname ()
{
    test_add_and_rm_other ("foo", "bar");
}

void test_rm_nonexistent_nonempty_prefix ()
{
    test_add_and_rm_other ("foobar", "foo");
}

void test_rm_nonexistent_nonempty_prefixed ()
{
    test_add_and_rm_other ("foo", "foobar");
}

void add_indexed_expect_unique (zlink::generic_mtrie_t<int> &mtrie_,
                                int *pipes_,
                                const char **names_,
                                size_t i_)
{
    const zlink::generic_mtrie_t<int>::prefix_t name_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (names_[i_]);

    bool res = mtrie_.add (name_data, getlen (name_data), &pipes_[i_]);
    TEST_ASSERT_TRUE (res);
}

void test_rm_nonexistent_between ()
{
    int pipes[3];
    const char *names[] = {"foo1", "foo2", "foo3"};

    zlink::generic_mtrie_t<int> mtrie;
    add_indexed_expect_unique (mtrie, pipes, names, 0);
    add_indexed_expect_unique (mtrie, pipes, names, 2);
    TEST_ASSERT_EQUAL_INT (2, mtrie.num_prefixes ());

    const zlink::generic_mtrie_t<int>::prefix_t name_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (names[1]);

    zlink::generic_mtrie_t<int>::rm_result res =
      mtrie.rm (name_data, getlen (name_data), &pipes[1]);
    TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::not_found, res);
    TEST_ASSERT_EQUAL_INT (2, mtrie.num_prefixes ());
}

template <size_t N>
void add_entries (zlink::generic_mtrie_t<int> &mtrie_, int (&pipes_)[N], const char *(&names_)[N])
{
    for (size_t i = 0; i < N; ++i) {
        add_indexed_expect_unique (mtrie_, pipes_, names_, i);
    }
    TEST_ASSERT_EQUAL_INT (N, mtrie_.num_prefixes ());
}

void test_add_multiple ()
{
    int pipes[3];
    const char *names[] = {"foo1", "foo2", "foo3"};

    zlink::generic_mtrie_t<int> mtrie;
    add_entries (mtrie, pipes, names);

    for (size_t i = 0; i < sizeof (names) / sizeof (names[0]); ++i) {
        const zlink::generic_mtrie_t<int>::prefix_t name_data =
          reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (names[i]);
        int count = 0;
        mtrie.match (name_data, getlen (name_data), mtrie_count, &count);
        TEST_ASSERT_EQUAL_INT (1, count);
    }
}

void test_add_multiple_reverse ()
{
    int pipes[3];
    const char *names[] = {"foo1", "foo2", "foo3"};

    zlink::generic_mtrie_t<int> mtrie;
    for (int i = 2; i >= 0; --i) {
        add_indexed_expect_unique (mtrie, pipes, names, static_cast<size_t> (i));
    }
    TEST_ASSERT_EQUAL_INT (3, mtrie.num_prefixes ());

    for (size_t i = 0; i < 3; ++i) {
        const zlink::generic_mtrie_t<int>::prefix_t name_data =
          reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (names[i]);
        int count = 0;
        mtrie.match (name_data, getlen (name_data), mtrie_count, &count);
        TEST_ASSERT_EQUAL_INT (1, count);
    }
}

template <size_t N> void add_and_rm_entries (const char *(&names_)[N])
{
    int pipes[N];
    zlink::generic_mtrie_t<int> mtrie;
    add_entries (mtrie, pipes, names_);

    for (size_t i = 0; i < N; ++i) {
        const zlink::generic_mtrie_t<int>::prefix_t name_data =
          reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (names_[i]);

        zlink::generic_mtrie_t<int>::rm_result res =
          mtrie.rm (name_data, getlen (name_data), &pipes[i]);
        TEST_ASSERT_EQUAL (zlink::generic_mtrie_t<int>::last_value_removed, res);
    }
    TEST_ASSERT_EQUAL_INT (0, mtrie.num_prefixes ());
}

void test_rm_multiple_in_order ()
{
    const char *names[] = {"foo1", "foo2", "foo3"};
    add_and_rm_entries (names);
}

void test_rm_multiple_reverse_order ()
{
    const char *names[] = {"foo3", "foo2", "foo1"};
    add_and_rm_entries (names);
}

void check_name (zlink::generic_mtrie_t<int>::prefix_t data_, size_t len_, const char *name_)
{
    TEST_ASSERT_EQUAL_UINT (strlen (name_), len_);
    TEST_ASSERT_EQUAL_STRING_LEN (name_, data_, len_);
}

template <size_t N> void add_entries_rm_pipes_unique (const char *(&names_)[N])
{
    int pipes[N];
    zlink::generic_mtrie_t<int> mtrie;
    add_entries (mtrie, pipes, names_);

    for (size_t i = 0; i < N; ++i) {
        mtrie.rm (&pipes[i], check_name, names_[i], false);
    }
}

void test_rm_with_callback_multiple_in_order ()
{
    const char *names[] = {"foo1", "foo2", "foo3"};

    add_entries_rm_pipes_unique (names);
}

void test_rm_with_callback_multiple_reverse_order ()
{
    const char *names[] = {"foo3", "foo2", "foo1"};

    add_entries_rm_pipes_unique (names);
}

void check_count (zlink::generic_mtrie_t<int>::prefix_t data_, size_t len_, int *count_)
{
    LIBZLINK_UNUSED (data_);
    LIBZLINK_UNUSED (len_);

    --(*count_);
    TEST_ASSERT_GREATER_OR_EQUAL (0, *count_);
}

void add_duplicate_entry (zlink::generic_mtrie_t<int> &mtrie_, int (&pipes_)[2])
{
    const char *name = "foo";

    const zlink::generic_mtrie_t<int>::prefix_t name_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (name);

    bool res = mtrie_.add (name_data, getlen (name_data), &pipes_[0]);
    TEST_ASSERT_TRUE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie_.num_prefixes ());
    res = mtrie_.add (name_data, getlen (name_data), &pipes_[1]);
    TEST_ASSERT_FALSE (res);
    TEST_ASSERT_EQUAL_INT (1, mtrie_.num_prefixes ());
}

void test_rm_with_callback_duplicate ()
{
    int pipes[2];
    zlink::generic_mtrie_t<int> mtrie;
    add_duplicate_entry (mtrie, pipes);

    int count = 1;
    mtrie.rm (&pipes[0], check_count, &count, false);
    count = 1;
    mtrie.rm (&pipes[1], check_count, &count, false);
}

void test_rm_with_callback_duplicate_uniq_only ()
{
    int pipes[2];
    zlink::generic_mtrie_t<int> mtrie;
    add_duplicate_entry (mtrie, pipes);

    int count = 0;
    mtrie.rm (&pipes[0], check_count, &count, true);
    count = 1;
    mtrie.rm (&pipes[1], check_count, &count, true);
}

void test_visit_values_deep_single_chain ()
{
    int pipe = 0;
    std::string topic (20000, 'a');
    zlink::generic_mtrie_t<int> mtrie;
    const zlink::generic_mtrie_t<int>::prefix_t topic_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (topic.data ());

    TEST_ASSERT_TRUE (mtrie.add (topic_data, topic.size (), &pipe));

    int count = 0;
    mtrie.visit_values (mtrie_count, &count);

    TEST_ASSERT_EQUAL_INT (1, count);
}

void test_destroy_deep_single_chain ()
{
    int pipe = 0;
    std::string topic (20000, 'b');
    const zlink::generic_mtrie_t<int>::prefix_t topic_data =
      reinterpret_cast<zlink::generic_mtrie_t<int>::prefix_t> (topic.data ());

    {
        zlink::generic_mtrie_t<int> mtrie;
        TEST_ASSERT_TRUE (mtrie.add (topic_data, topic.size (), &pipe));
    }
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_create);
    RUN_TEST (test_check_empty_match_nonempty_data);
    RUN_TEST (test_check_empty_match_empty_data);
    RUN_TEST (test_add_single_entry_match_exact);
    RUN_TEST (test_add_single_entry_twice_match_exact);
    RUN_TEST (test_add_rm_single_entry_match_exact);
    RUN_TEST (test_add_two_entries_match_prefix_and_exact);
    RUN_TEST (test_add_two_entries_with_same_name_match_exact);

    RUN_TEST (test_rm_nonexistent_0_size_empty);
    RUN_TEST (test_rm_nonexistent_empty);
    RUN_TEST (test_rm_nonexistent_nonempty_samename);
    RUN_TEST (test_rm_nonexistent_nonempty_differentname);
    RUN_TEST (test_rm_nonexistent_nonempty_prefix);
    RUN_TEST (test_rm_nonexistent_nonempty_prefixed);
    RUN_TEST (test_rm_nonexistent_between);

    RUN_TEST (test_add_multiple);
    RUN_TEST (test_add_multiple_reverse);
    RUN_TEST (test_rm_multiple_in_order);
    RUN_TEST (test_rm_multiple_reverse_order);

    RUN_TEST (test_rm_with_callback_multiple_in_order);
    RUN_TEST (test_rm_with_callback_multiple_reverse_order);
    RUN_TEST (test_rm_with_callback_duplicate);
    RUN_TEST (test_rm_with_callback_duplicate_uniq_only);
    RUN_TEST (test_visit_values_deep_single_chain);
    RUN_TEST (test_destroy_deep_single_chain);

    return UNITY_END ();
}
