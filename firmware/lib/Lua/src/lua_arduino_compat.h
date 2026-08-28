#pragma once
// Include before lua headers when Arduino is already in the TU.
// Arduino's min/max macros break Lua headers under C++.

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef abs
// keep abs for math; Lua does not need the macro form
#endif
