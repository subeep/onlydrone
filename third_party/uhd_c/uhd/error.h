/*
 * Copyright 2015 Ettus Research LLC
 * Copyright 2018 Ettus Research, a National Instruments Company
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stdlib.h>

//! UHD error codes
/*!
 * Each error code corresponds to a specific uhd::exception, with
 * extra codes corresponding to a boost::exception, std::exception,
 * and a catch-all for everything else. When an internal C++ function
 * throws an exception, UHD converts it to one of these error codes
 * to return on the C level.
 */
typedef enum {

    //! No error thrown.
    UHD_ERROR_NONE = 0,
    //! Invalid device arguments.
    UHD_ERROR_INVALID_DEVICE = 1,

    //! See uhd::index_error.
    UHD_ERROR_INDEX = 10,
    //! See uhd::key_error.
    UHD_ERROR_KEY = 11,

    //! See uhd::not_implemented_error.
    UHD_ERROR_NOT_IMPLEMENTED = 20,
    //! See uhd::usb_error.
    UHD_ERROR_USB = 21,

    //! See uhd::io_error.
    UHD_ERROR_IO = 30,
    //! See uhd::os_error.
    UHD_ERROR_OS = 31,

    //! See uhd::assertion_error.
    UHD_ERROR_ASSERTION = 40,
    //! See uhd::lookup_error.
    UHD_ERROR_LOOKUP = 41,
    //! See uhd::type_error.
    UHD_ERROR_TYPE = 42,
    //! See uhd::value_error.
    UHD_ERROR_VALUE = 43,
    //! See uhd::runtime_error.
    UHD_ERROR_RUNTIME = 44,
    //! See uhd::environment_error.
    UHD_ERROR_ENVIRONMENT = 45,
    //! See uhd::system_error.
    UHD_ERROR_SYSTEM = 46,
    //! See uhd::exception.
    UHD_ERROR_EXCEPT = 47,

    //! A boost::exception was thrown.
    UHD_ERROR_BOOSTEXCEPT = 60,

    //! A std::exception was thrown.
    UHD_ERROR_STDEXCEPT = 70,

    //! An unknown error was thrown.
    UHD_ERROR_UNKNOWN = 100
} uhd_error;


//! Return the last error string reported by UHD
/*!
 * Functions that do not take in UHD structs/handles will place any error
 * strings into a buffer that can be queried with this function. Functions that
 * do take in UHD structs/handles will place their error strings in both locations.
 */
UHD_API uhd_error uhd_get_last_error(
    char* error_out,
    size_t strbuffer_len
);
