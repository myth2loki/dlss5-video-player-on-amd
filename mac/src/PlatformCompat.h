#pragma once

// The single include a ported policy file needs.
//
// On Windows the policy layer gets its vocabulary for free: <windows.h> is
// pulled in by something, and the secure CRT's wide-string functions arrive
// with <cstdio> because MSVC puts them in the global namespace. Neither is true
// on macOS, and a file that only reaches them transitively - RenderPacePolicy.h
// uses swscanf_s with no include of its own - fails here for a reason that has
// nothing to do with its logic.
//
// Rather than edit each policy file to add an include Windows does not need,
// every ported file includes this one. It is the marker that says "this file is
// compiled on macOS", and it is the only place the substitution happens.
//
// A note on why this is a shim and not an abstraction: see UiTypes.h. The short
// version is that the policy layer is arithmetic, and 11.7k lines of arithmetic
// does not want an interface.

#include "UiTypes.h"
#include "UiTheme.h"
