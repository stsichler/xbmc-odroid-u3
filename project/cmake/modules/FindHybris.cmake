#.rst:
# FindHybris
# ------------
# Finds libhybris
#
# This will define the following variables::
#
# HYBRIS_FOUND - system has libhybris
# HYRBIS_INCLUDE_DIRS - the libhybris include directories
# HYBRIS_LIBRARIES - the libhybris libraries
# HYBRIS_DEFINITIONS - the libhybris definitions

if(NOT UNIX)
  message(FATAL_ERROR "libhybris is only available on Linux")
endif()

set(HYBRIS_DEFINITIONS -DTARGET_POSIX -DTARGET_LINUX -D_LINUX -DTARGET_HYBRIS -DHAS_HYBRIS)

pkg_check_modules(HYBRIS REQUIRED hwcomposer-egl>=0.1 hybris-egl-platform>=0.1 libhardware>=0.1 libsync>=0.1)

# NOTE: The following might be wrong if libhybris is not set up in the default
# location, but some files include files from the root of libhybris includes,
# so we must at least try to include libhybris's include root
list(APPEND HYBRIS_INCLUDE_DIRS /usr/include/hybris)
