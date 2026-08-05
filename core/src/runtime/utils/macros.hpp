
/* SPDX-License-Identifier: MPL-2.0 */

/******************************************************************************/
/*  0MQ Internal Use                                                          */
/******************************************************************************/

#define LIBZLINK_UNUSED(object) (void) object
#define LIBZLINK_DELETE(p_object)                                                                  \
    {                                                                                              \
        delete p_object;                                                                           \
        p_object = 0;                                                                              \
    }

/******************************************************************************/

#if !defined ZLINK_NOEXCEPT
#if defined ZLINK_HAVE_NOEXCEPT
#define ZLINK_NOEXCEPT noexcept
#else
#define ZLINK_NOEXCEPT
#endif
#endif

#if !defined ZLINK_OVERRIDE
#if defined ZLINK_HAVE_NOEXCEPT
#define ZLINK_OVERRIDE override
#else
#define ZLINK_OVERRIDE
#endif
#endif

#if !defined ZLINK_FINAL
#if defined ZLINK_HAVE_NOEXCEPT
#define ZLINK_FINAL final
#else
#define ZLINK_FINAL
#endif
#endif

#if !defined ZLINK_DEFAULT
#if defined ZLINK_HAVE_NOEXCEPT
#define ZLINK_DEFAULT = default;
#else
#define ZLINK_DEFAULT                                                                              \
    {                                                                                              \
    }
#endif
#endif

#if !defined ZLINK_NON_COPYABLE_NOR_MOVABLE
#if defined ZLINK_HAVE_NOEXCEPT
#define ZLINK_NON_COPYABLE_NOR_MOVABLE(classname)                                                  \
  public:                                                                                          \
    classname (const classname &) = delete;                                                        \
    classname &operator= (const classname &) = delete;                                             \
    classname (classname &&) = delete;                                                             \
    classname &operator= (classname &&) = delete;
#else
#define ZLINK_NON_COPYABLE_NOR_MOVABLE(classname)                                                  \
  private:                                                                                         \
    classname (const classname &);                                                                 \
    classname &operator= (const classname &);
#endif
#endif
