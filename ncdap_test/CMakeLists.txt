# Copyright 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001, 2002,
# 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014,
# 2015, 2016, 2017, 2018
# University Corporation for Atmospheric Research/Unidata.

# See netcdf-c/COPYRIGHT file for more info.
add_definitions(-D"TOPSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}/../")
add_definitions(-D"TOPBINDIR=${CMAKE_CURRENT_BINARY_DIR}/../")

remove_definitions(-DDLL_EXPORT)
# add_definitions(-D"TOPSRCDIR=${CMAKE_CURRENT_SOURCE_DIR}")

FILE(GLOB COPY_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.sh)
FILE(COPY ${COPY_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/ FILE_PERMISSIONS OWNER_WRITE OWNER_READ OWNER_EXECUTE)

IF(ENABLE_TESTS)

  IF(ENABLE_DAP_REMOTE_TESTS)
    BUILD_BIN_TEST(findtestserver)
    BUILD_BIN_TEST(pingurl)
  ENDIF()

  # Base tests
  # The tests are set up as a combination of shell scripts and executables that
  # must be run in a particular order. It is painful but will use macros to help
  # keep it from being too bad.
  # Binary Test Macro

  IF(BUILD_UTILITIES)
    add_sh_test(ncdap tst_ncdap3)
    IF(HAVE_BASH)
      SET_TESTS_PROPERTIES(ncdap_tst_ncdap3 PROPERTIES RUN_SERIAL TRUE)
    ENDIF(HAVE_BASH)
  ENDIF()

  IF(NOT MSVC)
    add_bin_env_test(ncdap t_dap3a)
    add_bin_env_test(ncdap test_cvt)
    add_bin_env_test(ncdap test_vara)
  ENDIF()

  IF(ENABLE_DAP_REMOTE_TESTS)

    IF(BUILD_UTILITIES)
      add_sh_test(ncdap tst_ber)
      add_sh_test(ncdap tst_remote3)
      IF(HAVE_BASH)
        SET_TESTS_PROPERTIES(ncdap_tst_remote3 PROPERTIES RUN_SERIAL TRUE)
      ENDIF(HAVE_BASH)
      add_sh_test(ncdap tst_zero_len_var)
      add_sh_test(ncdap tst_fillmismatch)
      IF(ENABLE_DAP_LONG_TESTS)
        add_sh_test(ncdap tst_longremote3)	
        SET_TESTS_PROPERTIES(ncdap_tst_longremote3 PROPERTIES RUN_SERIAL TRUE)
      ENDIF(ENABLE_DAP_LONG_TESTS)
      IF(TRUE)
        # Apparently iridl.ldeo.columbia.edu is down for now
        add_sh_test(ncdap tst_encode)
        # not yet fixed
        add_sh_test(ncdap tst_hyrax)
      ENDIF()
    ENDIF(BUILD_UTILITIES)
    IF(ENABLE_EXTERNAL_SERVER_TESTS)
          add_bin_test(ncdap test_manyurls)
          SET_TESTS_PROPERTIES(ncdap_test_manyurls PROPERTIES RUN_SERIAL TRUE)
    ENDIF()

    IF(ENABLE_DAP_AUTH_TESTS)
##obsolete      add_bin_test(ncdap t_auth)
      add_sh_test(ncdap testauth)
    ENDIF()

    add_bin_test(ncdap test_varm3)
    add_bin_test(ncdap test_nstride_cached)

    ###
    # This test relates to NCF-330 in
    # the Unidata JIRA system.
    ###
    #add_bin_test(ncdap t_ncf330)

  ENDIF()
ENDIF()

ADD_SUBDIRECTORY(testdata3)
ADD_SUBDIRECTORY(expected3)
ADD_SUBDIRECTORY(expectremote3)

## Specify files to be distributed by 'make dist'
FILE(GLOB CUR_EXTRA_DIST RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*.c ${CMAKE_CURRENT_SOURCE_DIR}/*.h ${CMAKE_CURRENT_SOURCE_DIR}/*.sh)
SET(CUR_EXTRA_DIST ${CUR_EXTRA_DIST} findtestserver.c.in CMakeLists.txt Makefile.am)
ADD_EXTRA_DIST("${CUR_EXTRA_DIST}")
