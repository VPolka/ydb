# Generated by devtools/yamaker.

LIBRARY()

LICENSE(BSD-3-Clause)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.17.0)

PEERDIR(
    contrib/libs/re2
)

ADDINCL(
    GLOBAL contrib/restricted/googletest/googletest/include
    contrib/restricted/googletest/googletest
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    GLOBAL -DGTEST_HAS_POSIX_RE=0
    GLOBAL -DGTEST_HAS_STD_WSTRING=1
    GLOBAL -DGTEST_USES_RE2=1
)

SRCS(
    src/gtest-all.cc
)

END()
