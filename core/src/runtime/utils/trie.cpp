/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "utils/err.hpp"
#include "utils/trie.hpp"

#include <stdlib.h>

#include <new>
#include <algorithm>
#include <vector>

namespace
{
const size_t initial_prefix_buffer_capacity = 256;
}

zlink::trie_t::trie_t () : _refcnt (0), _min (0), _count (0), _live_nodes (0)
{
}

zlink::trie_t::~trie_t ()
{
    std::vector<trie_t *> pending;
    trie_t *node = this;
    while (node) {
        trie_t *next = NULL;
        if (node->_count == 1) {
            zlink_assert (node->_next.node);
            next = node->_next.node;
        } else if (node->_count > 1) {
            for (unsigned short i = 0; i != node->_count; ++i)
                if (node->_next.table[i])
                    pending.push_back (node->_next.table[i]);
            free (node->_next.table);
        }
        node->_count = 0;
        node->_live_nodes = 0;
        node->_next.node = NULL;
        if (node != this)
            LIBZLINK_DELETE (node);
        if (!next && !pending.empty ()) {
            next = pending.back ();
            pending.pop_back ();
        }
        node = next;
    }
}

bool zlink::trie_t::add (unsigned char *prefix_, size_t size_)
{
    trie_t *node = this;
    while (size_) {
        const unsigned char c = *prefix_;
        if (c < node->_min || c >= node->_min + node->_count) {
            //  The character is out of range of currently handled
            //  characters. We have to extend the table.
            if (!node->_count) {
                node->_min = c;
                node->_count = 1;
                node->_next.node = NULL;
            } else if (node->_count == 1) {
                const unsigned char oldc = node->_min;
                trie_t *oldp = node->_next.node;
                node->_count = (node->_min < c ? c - node->_min : node->_min - c) + 1;
                node->_next.table =
                  static_cast<trie_t **> (malloc (sizeof (trie_t *) * node->_count));
                alloc_assert (node->_next.table);
                for (unsigned short i = 0; i != node->_count; ++i)
                    node->_next.table[i] = 0;
                node->_min = std::min (node->_min, c);
                node->_next.table[oldc - node->_min] = oldp;
            } else if (node->_min < c) {
                //  The new character is above the current character range.
                const unsigned short old_count = node->_count;
                node->_count = c - node->_min + 1;
                node->_next.table = static_cast<trie_t **> (
                  realloc (node->_next.table, sizeof (trie_t *) * node->_count));
                zlink_assert (node->_next.table);
                for (unsigned short i = old_count; i != node->_count; i++)
                    node->_next.table[i] = NULL;
            } else {
                //  The new character is below the current character range.
                const unsigned short old_count = node->_count;
                node->_count = (node->_min + old_count) - c;
                node->_next.table = static_cast<trie_t **> (
                  realloc (node->_next.table, sizeof (trie_t *) * node->_count));
                zlink_assert (node->_next.table);
                memmove (node->_next.table + node->_min - c, node->_next.table,
                         old_count * sizeof (trie_t *));
                for (unsigned short i = 0; i != node->_min - c; i++)
                    node->_next.table[i] = NULL;
                node->_min = c;
            }
        }

        //  If next node does not exist, create one.
        if (node->_count == 1) {
            if (!node->_next.node) {
                node->_next.node = new (std::nothrow) trie_t;
                alloc_assert (node->_next.node);
                ++node->_live_nodes;
                zlink_assert (node->_live_nodes == 1);
            }
            node = node->_next.node;
        } else {
            if (!node->_next.table[c - node->_min]) {
                node->_next.table[c - node->_min] = new (std::nothrow) trie_t;
                alloc_assert (node->_next.table[c - node->_min]);
                ++node->_live_nodes;
                zlink_assert (node->_live_nodes > 1);
            }
            node = node->_next.table[c - node->_min];
        }
        ++prefix_;
        --size_;
    }
    ++node->_refcnt;
    return node->_refcnt == 1;
}

bool zlink::trie_t::rm (unsigned char *prefix_, size_t size_)
{
    trie_t *node = this;
    trie_t *parent = this;
    unsigned char c = 0;
    for (size_t depth = 0; depth != size_; ++depth) {
        const unsigned char byte = prefix_[depth];
        if (!node->_count || byte < node->_min || byte >= node->_min + node->_count)
            return false;
        trie_t *child = node->_count == 1 ? node->_next.node : node->_next.table[byte - node->_min];
        if (!child)
            return false;
        if (!depth || node->_refcnt || node->_live_nodes > 1) {
            parent = node;
            c = byte;
        }
        node = child;
    }
    if (!node->_refcnt)
        return false;
    --node->_refcnt;
    const bool ret = node->_refcnt == 0;

    if (size_ && node->is_redundant ()) {
        node = parent->_count == 1 ? parent->_next.node : parent->_next.table[c - parent->_min];
        //  Prune redundant nodes.
        LIBZLINK_DELETE (node);
        zlink_assert (parent->_count > 0);

        if (parent->_count == 1) {
            //  The just pruned node was the only live node
            parent->_next.node = 0;
            parent->_count = 0;
            --parent->_live_nodes;
            zlink_assert (parent->_live_nodes == 0);
        } else {
            parent->_next.table[c - parent->_min] = 0;
            zlink_assert (parent->_live_nodes > 1);
            --parent->_live_nodes;

            //  Compact the table if possible
            if (parent->_live_nodes == 1) {
                //  We can switch to using the more compact single-node
                //  representation since the table only contains one live node
                trie_t *remaining_node = 0;
                //  Since we always compact the table the pruned node must
                //  either be the left-most or right-most ptr in the node
                //  table
                if (c == parent->_min) {
                    //  The pruned node is the left-most node ptr in the
                    //  node table => keep the right-most node
                    remaining_node = parent->_next.table[parent->_count - 1];
                    parent->_min += parent->_count - 1;
                } else if (c == parent->_min + parent->_count - 1) {
                    //  The pruned node is the right-most node ptr in the
                    //  node table => keep the left-most node
                    remaining_node = parent->_next.table[0];
                }
                zlink_assert (remaining_node);
                free (parent->_next.table);
                parent->_next.node = remaining_node;
                parent->_count = 1;
            } else if (c == parent->_min) {
                //  We can compact the table "from the left".
                //  Find the left-most non-null node ptr, which we'll use as
                //  our new min
                unsigned char new_min = parent->_min;
                for (unsigned short i = 1; i < parent->_count; ++i) {
                    if (parent->_next.table[i]) {
                        new_min = i + parent->_min;
                        break;
                    }
                }
                zlink_assert (new_min != parent->_min);

                trie_t **old_table = parent->_next.table;
                zlink_assert (new_min > parent->_min);
                zlink_assert (parent->_count > new_min - parent->_min);

                parent->_count = parent->_count - (new_min - parent->_min);
                parent->_next.table =
                  static_cast<trie_t **> (malloc (sizeof (trie_t *) * parent->_count));
                alloc_assert (parent->_next.table);

                memmove (parent->_next.table, old_table + (new_min - parent->_min),
                         sizeof (trie_t *) * parent->_count);
                free (old_table);

                parent->_min = new_min;
            } else if (c == parent->_min + parent->_count - 1) {
                //  We can compact the table "from the right".
                //  Find the right-most non-null node ptr, which we'll use to
                //  determine the new table size
                unsigned short new_count = parent->_count;
                for (unsigned short i = 1; i < parent->_count; ++i) {
                    if (parent->_next.table[parent->_count - 1 - i]) {
                        new_count = parent->_count - i;
                        break;
                    }
                }
                zlink_assert (new_count != parent->_count);
                parent->_count = new_count;

                trie_t **old_table = parent->_next.table;
                parent->_next.table =
                  static_cast<trie_t **> (malloc (sizeof (trie_t *) * parent->_count));
                alloc_assert (parent->_next.table);

                memmove (parent->_next.table, old_table, sizeof (trie_t *) * parent->_count);
                free (old_table);
            }
        }
    }
    return ret;
}

bool zlink::trie_t::check (const unsigned char *data_, size_t size_) const
{
    //  This function is on critical path. It deliberately doesn't use
    //  recursion to get a bit better performance.
    const trie_t *current = this;
    while (true) {
        //  We've found a corresponding subscription!
        if (current->_refcnt)
            return true;

        //  We've checked all the data and haven't found matching subscription.
        if (!size_)
            return false;

        //  If there's no corresponding slot for the first character
        //  of the prefix, the message does not match.
        const unsigned char c = *data_;
        if (c < current->_min || c >= current->_min + current->_count)
            return false;

        //  Move to the next character.
        if (current->_count == 1)
            current = current->_next.node;
        else {
            current = current->_next.table[c - current->_min];
            if (!current)
                return false;
        }
        data_++;
        size_--;
    }
}

void zlink::trie_t::apply (void (*func_) (unsigned char *data_, size_t size_, void *arg_),
                           void *arg_) const
{
    struct cursor_t
    {
        const trie_t *node;
        unsigned short child;
        size_t depth;
    };
    std::vector<cursor_t> parents;
    std::vector<unsigned char> prefix;
    if (_count)
        prefix.reserve (initial_prefix_buffer_capacity);
    const trie_t *node = this;
    unsigned short child = 0;
    while (true) {
        if (!child && node->_refcnt)
            func_ (prefix.empty () ? NULL : &prefix[0], prefix.size (), arg_);
        while (child < node->_count && node->_count > 1 && !node->_next.table[child])
            ++child;
        if (child < node->_count) {
            if (node->_count > 1 && child + 1 < node->_count) {
                const cursor_t parent = {node, static_cast<unsigned short> (child + 1),
                                         prefix.size ()};
                parents.push_back (parent);
            }
            prefix.push_back (static_cast<unsigned char> (node->_min + child));
            node = node->_count == 1 ? node->_next.node : node->_next.table[child];
            child = 0;
        } else {
            if (parents.empty ())
                break;
            const cursor_t parent = parents.back ();
            parents.pop_back ();
            prefix.resize (parent.depth);
            node = parent.node;
            child = parent.child;
        }
    }
}

bool zlink::trie_t::is_redundant () const
{
    return _refcnt == 0 && _live_nodes == 0;
}
