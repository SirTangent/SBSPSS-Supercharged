/*	Compiler-portability macros for the shim (M8 PR 4).

	The shim builds with MinGW g++ (the shipping toolchain) and with clang-cl
	targeting i686-pc-windows-msvc (port/cmake/clangcl-toolchain.cmake: a
	Visual Studio-debuggable build that does not depend on MSYS2's shrinking
	32-bit repository).  Everything compiler-specific the shim needs lives
	here so no other TU tests __GNUC__ / _MSC_VER itself.

	args.cpp includes this by its bare name: psyq_args carries no include
	set (see CMakeLists.txt), so the header must stay self-contained.
*/
#ifndef PORT_COMPILER_H
#define PORT_COMPILER_H

/*	PORT_EARLY_CTOR(fn): define `static void fn(void)` and have the CRT run
	it before every normal-priority static constructor in the program.

		PORT_EARLY_CTOR(seedGuard)
		{
			...
		}

	GNU: constructor priority 101 (0-100 are reserved).  MSVC: a function
	pointer in the .CRT$XCT initializer slot - the CRT walks .CRT$XCA..XCZ
	in order and compiler-generated static constructors sit in .CRT$XCU, so
	XCT runs first.  clang-cl takes the MSVC path (it defines _MSC_VER); the
	pointer is marked `used` there because an unreferenced internal global
	is otherwise fair game for the optimizer, whereas MSVC never drops
	__declspec(allocate) data.  */
#if defined(_MSC_VER)
#if defined(__clang__)
#define PORT_KEEP_	__attribute__((used))
#else
#define PORT_KEEP_
#endif
#pragma section(".CRT$XCT", read)
#define PORT_EARLY_CTOR(fn) \
	static void fn(void); \
	__declspec(allocate(".CRT$XCT")) PORT_KEEP_ static void (*const fn##_xct)(void) = fn; \
	static void fn(void)
#else
#define PORT_EARLY_CTOR(fn) \
	__attribute__((constructor(101))) static void fn(void)
#endif

/*	PORT_NORETURN: prefix for a function declaration that never returns.  */
#if defined(_MSC_VER)
#define PORT_NORETURN	__declspec(noreturn)
#else
#define PORT_NORETURN	__attribute__((noreturn))
#endif

#endif
