#include <BaseTsd.h>

typedef SSIZE_T ssize_t;

#define ENCHANT_MAJOR_VERSION "2"
#define ENCHANT_VERSION_STRING "2.2.8"

#undef ENABLE_RELOCATABLE
#undef HAVE_FLOCK

#define _GL_ATTRIBUTE_PURE
#define _GL_ATTRIBUTE_CONST

#if defined(_MSC_VER)
# pragma warning(disable: 4996) /* The POSIX name for this item is deprecated. Instead, use the ISO C++ conformant name. */
#endif
