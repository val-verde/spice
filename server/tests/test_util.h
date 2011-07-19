#ifndef __TEST_UTIL_H__
#define __TEST_UTIL_H__

#define TRUE 1
#define FALSE 0

#ifdef ASSERT
#undef ASSERT
#endif
#define ASSERT(x) if (!(x)) {                               \
    printf("%s: ASSERT %s failed\n", __FUNCTION__, #x);     \
    abort();                                                \
}

#endif // __TEST_UTIL_H__
