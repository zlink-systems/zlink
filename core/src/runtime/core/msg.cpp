/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/compat.hpp"
#include "utils/macros.hpp"
#include "core/msg.hpp"

#include <string.h>
#include <stdlib.h>
#include <new>

#include "utils/stdint.hpp"
#include "utils/likely.hpp"
#include "utils/err.hpp"
#include "utils/env.hpp"

//  Check whether the sizes of public representation of the message (zlink_msg_t)
//  and private representation of the message (zlink::msg_t) match.

typedef char zlink_msg_size_check[2 * ((sizeof (zlink::msg_t) == sizeof (zlink_msg_t)) != 0) - 1];

namespace
{
const uintptr_t slice_lmsg_flag = static_cast<uintptr_t> (1);

const size_t slice_content_pool_max =
  static_cast<size_t> (zlink::env::positive_int ("ZLINK_MSG_SLICE_CONTENT_POOL_MAX", 32768));

//  Frees cached entries at thread exit; without this the pooled contents
//  become unreachable when the owning thread's storage is destroyed.
struct slice_content_pool_t : std::vector<zlink::msg_t::content_t *>
{
    ~slice_content_pool_t ()
    {
        for (size_t i = 0; i < size (); ++i)
            std::free ((*this)[i]);
    }
};

slice_content_pool_t &slice_content_pool ()
{
    static thread_local slice_content_pool_t pool;
    return pool;
}

zlink::msg_t::content_t *acquire_slice_content ()
{
    slice_content_pool_t &pool = slice_content_pool ();
    if (!pool.empty ()) {
        zlink::msg_t::content_t *content = pool.back ();
        pool.pop_back ();
        return content;
    }

    zlink::msg_t::content_t *content =
      static_cast<zlink::msg_t::content_t *> (std::malloc (sizeof (zlink::msg_t::content_t)));
    if (!content)
        errno = ENOMEM;
    return content;
}

void release_slice_content (zlink::msg_t::content_t *content_)
{
    if (!content_)
        return;

    slice_content_pool_t &pool = slice_content_pool ();
    if (pool.size () < slice_content_pool_max)
        pool.push_back (content_);
    else
        std::free (content_);
}

void *encode_slice_hint (zlink::msg_t::content_t *content_, bool lmsg_owner_)
{
    return reinterpret_cast<void *> (reinterpret_cast<uintptr_t> (content_)
                                     | (lmsg_owner_ ? slice_lmsg_flag : 0));
}

zlink::msg_t::content_t *decode_slice_hint (void *hint_, bool *lmsg_owner_out_)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t> (hint_);
    if (lmsg_owner_out_)
        *lmsg_owner_out_ = (encoded & slice_lmsg_flag) != 0;
    return reinterpret_cast<zlink::msg_t::content_t *> (encoded & ~slice_lmsg_flag);
}
}

bool zlink::msg_t::check () const
{
    return _u.base.type >= type_min && _u.base.type <= type_max;
}

int zlink::msg_t::init (
  void *data_, size_t size_, msg_free_fn *ffn_, void *hint_, content_t *content_)
{
    if (size_ < max_vsm_size) {
        const int rc = init_size (size_);

        if (rc != -1) {
            memcpy (data (), data_, size_);
            return 0;
        }
        return -1;
    }
    if (content_) {
        return init_external_storage (content_, data_, size_, ffn_, hint_);
    }
    return init_data (data_, size_, ffn_, hint_);
}

int zlink::msg_t::init ()
{
    _u.vsm.type = type_vsm;
    _u.vsm.flags = 0;
    _u.vsm.size = 0;
    _u.vsm.group.sgroup.group[0] = '\0';
    _u.vsm.group.type = group_type_short;
    _u.vsm.routing_id = 0;
    _u.vsm.transport_connection_id = 0;
    return 0;
}

int zlink::msg_t::init_size (size_t size_)
{
    if (size_ <= max_vsm_size) {
        _u.vsm.type = type_vsm;
        _u.vsm.flags = 0;
        _u.vsm.size = static_cast<unsigned char> (size_);
        _u.vsm.group.sgroup.group[0] = '\0';
        _u.vsm.group.type = group_type_short;
        _u.vsm.routing_id = 0;
        _u.vsm.transport_connection_id = 0;
    } else {
        _u.lmsg.type = type_lmsg;
        _u.lmsg.flags = 0;
        _u.lmsg.group.sgroup.group[0] = '\0';
        _u.lmsg.group.type = group_type_short;
        _u.lmsg.routing_id = 0;
        _u.lmsg.transport_connection_id = 0;
        _u.lmsg.content = NULL;
        if (sizeof (content_t) + size_ > size_)
            _u.lmsg.content = static_cast<content_t *> (malloc (sizeof (content_t) + size_));
        if (unlikely (!_u.lmsg.content)) {
            errno = ENOMEM;
            return -1;
        }

        _u.lmsg.content->data = _u.lmsg.content + 1;
        _u.lmsg.content->size = size_;
        _u.lmsg.content->ffn = NULL;
        _u.lmsg.content->hint = NULL;
        new (&_u.lmsg.content->refcnt) zlink::atomic_counter_t ();
    }
    return 0;
}

int zlink::msg_t::init_buffer (const void *buf_, size_t size_)
{
    const int rc = init_size (size_);
    if (unlikely (rc < 0)) {
        return -1;
    }
    if (size_) {
        // NULL and zero size is allowed
        assert (NULL != buf_);
        memcpy (data (), buf_, size_);
    }
    return 0;
}

void zlink::msg_t::call_dec_ref_on_slice (void *data_, void *hint_)
{
    LIBZLINK_UNUSED (data_);

    if (!hint_)
        return;

    bool lmsg_owner = false;
    content_t *content = decode_slice_hint (hint_, &lmsg_owner);
    if (!content)
        return;

    if (!content->refcnt.sub (1)) {
        content->refcnt.~atomic_counter_t ();
        if (content->ffn)
            content->ffn (content->data, content->hint);
        if (lmsg_owner)
            free (content);
    }
}

int zlink::msg_t::init_view (msg_t &src_, size_t offset_, size_t size_)
{
    //  Check the validity of the source.
    if (unlikely (!src_.check ())) {
        errno = EFAULT;
        return -1;
    }

    const size_t src_size = src_.size ();
    if (offset_ > src_size || size_ > src_size - offset_) {
        errno = EINVAL;
        return -1;
    }

    int rc = close ();
    if (unlikely (rc < 0))
        return rc;

    if (size_ == 0)
        return init_size (0);

    unsigned char *src_data = static_cast<unsigned char *> (src_.data ());
    if (!src_data) {
        errno = EFAULT;
        return -1;
    }
    void *view_data = src_data + offset_;

    if (src_.is_lmsg () || src_.is_zcmsg ()) {
        content_t *content = src_.is_lmsg () ? src_._u.lmsg.content : src_._u.zclmsg.content;
        if (!content) {
            errno = EFAULT;
            return -1;
        }

        const bool promoted_to_shared = !(src_._u.base.flags & msg_t::shared);
        if (!promoted_to_shared)
            content->refcnt.add (1);
        else {
            content->refcnt.set (2);
            src_._u.base.flags |= msg_t::shared;
        }

        const bool lmsg_owner = src_.is_lmsg ();
        content_t *view_content = acquire_slice_content ();
        if (!view_content) {
            if (!content->refcnt.sub (1)) {
                content->refcnt.~atomic_counter_t ();
                if (content->ffn)
                    content->ffn (content->data, content->hint);
                if (lmsg_owner)
                    free (content);
            } else if (promoted_to_shared) {
                src_._u.base.flags &= ~msg_t::shared;
            }
            const int init_rc = init ();
            errno_assert (init_rc == 0);
            return -1;
        }

        rc = init_external_storage (view_content, view_data, size_, &msg_t::call_dec_ref_on_slice,
                                    encode_slice_hint (content, lmsg_owner));
        if (likely (rc == 0))
            return 0;

        const int saved_errno = errno;
        release_slice_content (view_content);
        if (!content->refcnt.sub (1)) {
            content->refcnt.~atomic_counter_t ();
            if (content->ffn)
                content->ffn (content->data, content->hint);
            if (lmsg_owner)
                free (content);
        } else if (promoted_to_shared) {
            src_._u.base.flags &= ~msg_t::shared;
        }
        const int init_rc = init ();
        errno_assert (init_rc == 0);
        errno = saved_errno;
        return -1;
    }

    rc = init_size (size_);
    if (unlikely (rc < 0)) {
        const int saved_errno = errno;
        const int init_rc = init ();
        errno_assert (init_rc == 0);
        errno = saved_errno;
        return -1;
    }

    memcpy (data (), view_data, size_);
    return 0;
}

int zlink::msg_t::init_external_storage (
  content_t *content_, void *data_, size_t size_, msg_free_fn *ffn_, void *hint_)
{
    zlink_assert (NULL != data_);
    zlink_assert (NULL != content_);

    _u.zclmsg.type = type_zclmsg;
    _u.zclmsg.flags = 0;
    _u.zclmsg.group.sgroup.group[0] = '\0';
    _u.zclmsg.group.type = group_type_short;
    _u.zclmsg.routing_id = 0;
    _u.zclmsg.transport_connection_id = 0;

    _u.zclmsg.content = content_;
    _u.zclmsg.content->data = data_;
    _u.zclmsg.content->size = size_;
    _u.zclmsg.content->ffn = ffn_;
    _u.zclmsg.content->hint = hint_;
    new (&_u.zclmsg.content->refcnt) zlink::atomic_counter_t ();

    return 0;
}

int zlink::msg_t::init_data (void *data_, size_t size_, msg_free_fn *ffn_, void *hint_)
{
    //  If data is NULL and size is not 0, a segfault
    //  would occur once the data is accessed
    zlink_assert (data_ != NULL || size_ == 0);

    //  Initialize constant message if there's no need to deallocate
    if (ffn_ == NULL) {
        _u.cmsg.type = type_cmsg;
        _u.cmsg.flags = 0;
        _u.cmsg.data = data_;
        _u.cmsg.size = size_;
        _u.cmsg.group.sgroup.group[0] = '\0';
        _u.cmsg.group.type = group_type_short;
        _u.cmsg.routing_id = 0;
        _u.cmsg.transport_connection_id = 0;
    } else {
        _u.lmsg.type = type_lmsg;
        _u.lmsg.flags = 0;
        _u.lmsg.group.sgroup.group[0] = '\0';
        _u.lmsg.group.type = group_type_short;
        _u.lmsg.routing_id = 0;
        _u.lmsg.transport_connection_id = 0;
        _u.lmsg.content = static_cast<content_t *> (malloc (sizeof (content_t)));
        if (!_u.lmsg.content) {
            errno = ENOMEM;
            return -1;
        }

        _u.lmsg.content->data = data_;
        _u.lmsg.content->size = size_;
        _u.lmsg.content->ffn = ffn_;
        _u.lmsg.content->hint = hint_;
        new (&_u.lmsg.content->refcnt) zlink::atomic_counter_t ();
    }
    return 0;
}

int zlink::msg_t::init_delimiter ()
{
    _u.delimiter.type = type_delimiter;
    _u.delimiter.flags = 0;
    _u.delimiter.group.sgroup.group[0] = '\0';
    _u.delimiter.group.type = group_type_short;
    _u.delimiter.routing_id = 0;
    _u.delimiter.transport_connection_id = 0;
    return 0;
}

int zlink::msg_t::init_join ()
{
    _u.base.type = type_join;
    _u.base.flags = 0;
    _u.base.group.sgroup.group[0] = '\0';
    _u.base.group.type = group_type_short;
    _u.base.routing_id = 0;
    _u.base.transport_connection_id = 0;
    return 0;
}

int zlink::msg_t::init_leave ()
{
    _u.base.type = type_leave;
    _u.base.flags = 0;
    _u.base.group.sgroup.group[0] = '\0';
    _u.base.group.type = group_type_short;
    _u.base.routing_id = 0;
    _u.base.transport_connection_id = 0;
    return 0;
}

int zlink::msg_t::init_subscribe (const size_t size_, const unsigned char *topic_)
{
    int rc = init_size (size_);
    if (rc == 0) {
        set_flags (zlink::msg_t::subscribe);

        //  We explicitly allow a NULL subscription with size zero
        if (size_) {
            assert (topic_);
            memcpy (data (), topic_, size_);
        }
    }
    return rc;
}

int zlink::msg_t::init_cancel (const size_t size_, const unsigned char *topic_)
{
    int rc = init_size (size_);
    if (rc == 0) {
        set_flags (zlink::msg_t::cancel);

        //  We explicitly allow a NULL subscription with size zero
        if (size_) {
            assert (topic_);
            memcpy (data (), topic_, size_);
        }
    }
    return rc;
}

int zlink::msg_t::close ()
{
    //  Check the validity of the message.
    if (unlikely (!check ())) {
        errno = EFAULT;
        return -1;
    }

    if (_u.base.type == type_lmsg) {
        //  If the content is not shared, or if it is shared and the reference
        //  count has dropped to zero, deallocate it.
        if (!(_u.lmsg.flags & msg_t::shared) || !_u.lmsg.content->refcnt.sub (1)) {
            //  We used "placement new" operator to initialize the reference
            //  counter so we call the destructor explicitly now.
            _u.lmsg.content->refcnt.~atomic_counter_t ();

            if (_u.lmsg.content->ffn)
                _u.lmsg.content->ffn (_u.lmsg.content->data, _u.lmsg.content->hint);
            free (_u.lmsg.content);
        }
    }

    if (is_zcmsg ()) {
        zlink_assert (_u.zclmsg.content->ffn);
        content_t *content = _u.zclmsg.content;
        msg_free_fn *ffn = content->ffn;
        const bool pooled_slice_content = ffn == &msg_t::call_dec_ref_on_slice;

        //  If the content is not shared, or if it is shared and the reference
        //  count has dropped to zero, deallocate it.
        if (!(_u.zclmsg.flags & msg_t::shared) || !content->refcnt.sub (1)) {
            //  We used "placement new" operator to initialize the reference
            //  counter so we call the destructor explicitly now.
            content->refcnt.~atomic_counter_t ();

            ffn (content->data, content->hint);
            if (pooled_slice_content)
                release_slice_content (content);
        }
    }

    if (_u.base.group.type == group_type_long) {
        if (!_u.base.group.lgroup.content->refcnt.sub (1)) {
            //  We used "placement new" operator to initialize the reference
            //  counter so we call the destructor explicitly now.
            _u.base.group.lgroup.content->refcnt.~atomic_counter_t ();

            free (_u.base.group.lgroup.content);
        }
    }

    //  Make the message invalid.
    _u.base.type = 0;

    return 0;
}

int zlink::msg_t::move (msg_t &src_)
{
    //  Check the validity of the source.
    if (unlikely (!src_.check ())) {
        errno = EFAULT;
        return -1;
    }

    if (check ()) {
        int rc = close ();
        if (unlikely (rc < 0))
            return rc;
    }

    *this = src_;

    const int rc = src_.init ();
    if (unlikely (rc < 0))
        return rc;

    return 0;
}

int zlink::msg_t::copy (msg_t &src_)
{
    //  Check the validity of the source.
    if (unlikely (!src_.check ())) {
        errno = EFAULT;
        return -1;
    }

    const int rc = close ();
    if (unlikely (rc < 0))
        return rc;

    // The initial reference count, when a non-shared message is initially
    // shared (between the original and the copy we create here).
    const atomic_counter_t::integer_t initial_shared_refcnt = 2;

    if (src_.is_lmsg () || src_.is_zcmsg ()) {
        //  One reference is added to shared messages. Non-shared messages
        //  are turned into shared messages.
        if (src_.flags () & msg_t::shared)
            src_.refcnt ()->add (1);
        else {
            src_.set_flags (msg_t::shared);
            src_.refcnt ()->set (initial_shared_refcnt);
        }
    }

    if (src_._u.base.group.type == group_type_long)
        src_._u.base.group.lgroup.content->refcnt.add (1);

    *this = src_;

    return 0;
}

uint32_t zlink::msg_t::refcnt_value () const
{
    //  Check the validity of the message.
    zlink_assert (check ());

    if (_u.base.type == type_lmsg) {
        if (_u.lmsg.flags & msg_t::shared)
            return _u.lmsg.content->refcnt.get ();
        return 1;
    }

    if (_u.base.type == type_zclmsg) {
        if (_u.zclmsg.flags & msg_t::shared)
            return _u.zclmsg.content->refcnt.get ();
        return 1;
    }

    return 1;
}

void zlink::msg_t::shrink (size_t new_size_)
{
    //  Check the validity of the message.
    zlink_assert (check ());
    zlink_assert (new_size_ <= size ());

    switch (_u.base.type) {
        case type_vsm:
            _u.vsm.size = static_cast<unsigned char> (new_size_);
            break;
        case type_lmsg:
            _u.lmsg.content->size = new_size_;
            break;
        case type_zclmsg:
            _u.zclmsg.content->size = new_size_;
            break;
        case type_cmsg:
            _u.cmsg.size = new_size_;
            break;
        default:
            zlink_assert (false);
    }
}

unsigned char zlink::msg_t::flags () const
{
    return _u.base.flags;
}

void zlink::msg_t::set_flags (unsigned char flags_)
{
    _u.base.flags |= flags_;
}

void zlink::msg_t::reset_flags (unsigned char flags_)
{
    _u.base.flags &= ~flags_;
}

bool zlink::msg_t::is_routing_id () const
{
    return (_u.base.flags & routing_id) == routing_id;
}

bool zlink::msg_t::is_credential () const
{
    return (_u.base.flags & credential) == credential;
}

bool zlink::msg_t::is_delimiter () const
{
    return _u.base.type == type_delimiter;
}

bool zlink::msg_t::is_vsm () const
{
    return _u.base.type == type_vsm;
}

bool zlink::msg_t::is_cmsg () const
{
    return _u.base.type == type_cmsg;
}

bool zlink::msg_t::is_lmsg () const
{
    return _u.base.type == type_lmsg;
}

bool zlink::msg_t::is_zcmsg () const
{
    return _u.base.type == type_zclmsg;
}

bool zlink::msg_t::is_join () const
{
    return _u.base.type == type_join;
}

bool zlink::msg_t::is_leave () const
{
    return _u.base.type == type_leave;
}

bool zlink::msg_t::is_ping () const
{
    return (_u.base.flags & CMD_TYPE_MASK) == ping;
}

bool zlink::msg_t::is_pong () const
{
    return (_u.base.flags & CMD_TYPE_MASK) == pong;
}

bool zlink::msg_t::is_close_cmd () const
{
    return (_u.base.flags & CMD_TYPE_MASK) == close_cmd;
}

namespace
{
size_t command_body_size_after_prefix (const zlink::msg_t &msg_, const size_t prefix_size_)
{
    const size_t size = msg_.size ();
    return size >= prefix_size_ ? size - prefix_size_ : 0;
}

unsigned char *command_body_after_prefix (zlink::msg_t *msg_, const size_t prefix_size_)
{
    if (msg_->size () < prefix_size_)
        return NULL;
    return static_cast<unsigned char *> (msg_->data ()) + prefix_size_;
}
}

size_t zlink::msg_t::command_body_size () const
{
    if (this->is_ping () || this->is_pong ())
        return command_body_size_after_prefix (*this, ping_cmd_name_size);
    else if (!(this->flags () & msg_t::command) && (this->is_subscribe () || this->is_cancel ()))
        return this->size ();
    else if (this->is_subscribe ())
        return command_body_size_after_prefix (*this, sub_cmd_name_size);
    else if (this->is_cancel ())
        return command_body_size_after_prefix (*this, cancel_cmd_name_size);

    return 0;
}

void *zlink::msg_t::command_body ()
{
    unsigned char *data = NULL;

    if (this->is_ping () || this->is_pong ())
        data = command_body_after_prefix (this, ping_cmd_name_size);
    //  With inproc, command flag is not set for sub/cancel
    else if (!(this->flags () & msg_t::command) && (this->is_subscribe () || this->is_cancel ()))
        data = static_cast<unsigned char *> (this->data ());
    else if (this->is_subscribe ())
        data = command_body_after_prefix (this, sub_cmd_name_size);
    else if (this->is_cancel ())
        data = command_body_after_prefix (this, cancel_cmd_name_size);

    return data;
}

void zlink::msg_t::add_refs (int refs_)
{
    zlink_assert (refs_ >= 0);

    //  No copies required.
    if (!refs_)
        return;

    //  VSMs, CMSGS and delimiters can be copied straight away. The only
    //  message type that needs special care are long messages.
    if (_u.base.type == type_lmsg || is_zcmsg ()) {
        if (_u.base.flags & msg_t::shared)
            refcnt ()->add (refs_);
        else {
            refcnt ()->set (refs_ + 1);
            _u.base.flags |= msg_t::shared;
        }
    }
}

bool zlink::msg_t::rm_refs (int refs_)
{
    zlink_assert (refs_ >= 0);

    //  No copies required.
    if (!refs_)
        return true;

    //  If there's only one reference close the message.
    if ((_u.base.type != type_zclmsg && _u.base.type != type_lmsg)
        || !(_u.base.flags & msg_t::shared)) {
        close ();
        return false;
    }

    //  The only message type that needs special care are long and zcopy messages.
    if (_u.base.type == type_lmsg && !_u.lmsg.content->refcnt.sub (refs_)) {
        //  We used "placement new" operator to initialize the reference
        //  counter so we call the destructor explicitly now.
        _u.lmsg.content->refcnt.~atomic_counter_t ();

        if (_u.lmsg.content->ffn)
            _u.lmsg.content->ffn (_u.lmsg.content->data, _u.lmsg.content->hint);
        free (_u.lmsg.content);

        return false;
    }

    if (is_zcmsg () && !_u.zclmsg.content->refcnt.sub (refs_)) {
        // storage for rfcnt is provided externally
        msg_free_fn *ffn = _u.zclmsg.content->ffn;
        const bool pooled_slice_content = ffn == &msg_t::call_dec_ref_on_slice;
        if (ffn) {
            ffn (_u.zclmsg.content->data, _u.zclmsg.content->hint);
            if (pooled_slice_content)
                release_slice_content (_u.zclmsg.content);
        }

        return false;
    }

    return true;
}

uint32_t zlink::msg_t::get_routing_id () const
{
    return _u.base.routing_id;
}

int zlink::msg_t::set_routing_id (uint32_t routing_id_)
{
    if (routing_id_) {
        _u.base.routing_id = routing_id_;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int zlink::msg_t::reset_routing_id ()
{
    _u.base.routing_id = 0;
    return 0;
}

uint64_t zlink::msg_t::transport_connection_id () const
{
    return _u.base.transport_connection_id;
}

void zlink::msg_t::set_transport_connection_id (uint64_t connection_id_)
{
    _u.base.transport_connection_id = connection_id_;
}

const char *zlink::msg_t::group () const
{
    if (_u.base.group.type == group_type_long)
        return _u.base.group.lgroup.content->group;
    return _u.base.group.sgroup.group;
}

int zlink::msg_t::set_group (const char *group_)
{
    size_t length = strnlen (group_, ZLINK_GROUP_MAX_LENGTH);

    return set_group (group_, length);
}

int zlink::msg_t::set_group (const char *group_, size_t length_)
{
    if (length_ > ZLINK_GROUP_MAX_LENGTH) {
        errno = EINVAL;
        return -1;
    }

    if (length_ > 14) {
        _u.base.group.lgroup.type = group_type_long;
        _u.base.group.lgroup.content = (long_group_t *) malloc (sizeof (long_group_t));
        assert (_u.base.group.lgroup.content);
        new (&_u.base.group.lgroup.content->refcnt) zlink::atomic_counter_t ();
        _u.base.group.lgroup.content->refcnt.set (1);
        strncpy (_u.base.group.lgroup.content->group, group_, length_);
        _u.base.group.lgroup.content->group[length_] = '\0';
    } else {
        strncpy (_u.base.group.sgroup.group, group_, length_);
        _u.base.group.sgroup.group[length_] = '\0';
    }

    return 0;
}

zlink::atomic_counter_t *zlink::msg_t::refcnt ()
{
    switch (_u.base.type) {
        case type_lmsg:
            return &_u.lmsg.content->refcnt;
        case type_zclmsg:
            return &_u.zclmsg.content->refcnt;
        default:
            zlink_assert (false);
            return NULL;
    }
}
