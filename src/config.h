#ifndef _WIN32
# if defined(__GNUC__) || defined(__clang__)
#  include_next <config.h>
# endif
#else
#include <BaseTsd.h>

typedef SSIZE_T ssize_t;

#define _GL_CONFIG_H_INCLUDED 1
#define PACKAGE "enchant"
#define PACKAGE_VERSION "2.8.15"
#define ENCHANT_MAJOR_VERSION "2"
#define ENCHANT_VERSION_STRING "2.8.15"

#define ENABLE_RELOCATABLE 0
#define HAVE_FLOCK 0

#define _GL_ATTRIBUTE_PURE
#define _GL_ATTRIBUTE_CONST
#define _GL_UNUSED

#if defined(_MSC_VER)
# pragma warning(disable: 4996) /* The POSIX name for this item is deprecated. Instead, use the ISO C++ conformant name. */
#endif
#endif
