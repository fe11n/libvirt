AC_DEFUN([LIBVIRT_DRIVER_ARG_STRATOVIRT], [
  LIBVIRT_ARG_WITH_FEATURE([STRATOVIRT], [QEMU/KVM], [yes])
])

AC_DEFUN([LIBVIRT_DRIVER_CHECK_STRATOVIRT], [
  if test "$with_stratovirt" = "yes"; then
    AC_DEFINE_UNQUOTED([WITH_STRATOVIRT], 1, [whether STRATOVIRT driver is enabled])
  fi
  AM_CONDITIONAL([WITH_STRATOVIRT], [test "$with_stratovirt" = "yes"])

  AC_DEFINE_UNQUOTED([STRATOVIRT_USER], ["root"], [STRATOVIRT user account])
  AC_DEFINE_UNQUOTED([STRATOVIRT_GROUP], ["root"], [STRATOVIRT group account])
])

AC_DEFUN([LIBVIRT_DRIVER_RESULT_STRATOVIRT], [
  LIBVIRT_RESULT([STRATOVIRT], [$with_stratovirt])
])
