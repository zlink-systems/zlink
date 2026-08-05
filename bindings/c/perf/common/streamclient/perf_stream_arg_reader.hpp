#ifndef PERF_STREAM_ARG_READER_HPP
#define PERF_STREAM_ARG_READER_HPP

// Simple "--key value" command-line argument reader.
// Scans argv for matching keys and returns the next token as value.

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

class arg_reader_t
{
  public:
    arg_reader_t (int argc_, char **argv_) : argc (argc_), argv (argv_) {}

    // Lookup "--key value" pair. Returns fallback if key is not found.
    std::string get_string (const char *key, const char *fallback) const
    {
        if (!key)
            return fallback ? std::string (fallback) : std::string ();

        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp (argv[i], key) == 0)
                return std::string (argv[i + 1]);
        }
        return fallback ? std::string (fallback) : std::string ();
    }

    // Lookup "--key value" as int. Clamps to [min_value, INT_MAX].
    int get_int (const char *key, int fallback, int min_value) const
    {
        const std::string v = get_string (key, "");
        if (v.empty ())
            return fallback;

        char *end = NULL;
        const long parsed = std::strtol (v.c_str (), &end, 10);
        if (end == v.c_str ())
            return fallback;
        if (parsed < static_cast<long> (min_value))
            return min_value;
        if (parsed > static_cast<long> (std::numeric_limits<int>::max ()))
            return std::numeric_limits<int>::max ();
        return static_cast<int> (parsed);
    }

  private:
    int argc;
    char **argv;
};

#endif
