# UHD C declarations

Derived from the installed Ubuntu UHD **4.1.0.5-3** development headers
(`UHD_VERSION=4010005`), upstream https://github.com/EttusResearch/uhd.
Copyright notices and GPL-3.0-or-later licensing remain in each header.
Only `__cplusplus` sections (C++ implementation helpers and linkage guards) were
removed with `unifdef -U__cplusplus`. No struct, enum or C signature is rewritten.
Regenerate with `scripts/vendor_uhd_c.py /usr/include /path/to/unifdef`.

The adapter wraps declarations in `extern "C"` and uses their exact pointer types
with runtime symbol loading. It never links MSVC C++ objects to MinGW objects.
All UHD objects and returned info allocations are freed through UHD functions.
Runtime support targets 64-bit UHD 4.x; required exports are checked before device
creation. New runtime versions still require hardware acceptance testing.
