/*	Compiler-portability macros for the shim (M8 PR 4).

	The shim builds with clang-cl, for x86_64- and i686-pc-windows-msvc
	(port/cmake/clangcl-toolchain.cmake).  MinGW g++ built it too until
	issue #39, and the GNU spellings it needed are gone.  Everything
	compiler-specific the shim needs lives here so no other TU tests
	_MSC_VER itself.

	args.cpp includes this by its bare name: psyq_args carries no include
	set (see CMakeLists.txt), so the header must stay self-contained.
*/
#ifndef PORT_COMPILER_H
#define PORT_COMPILER_H

#if !defined(_MSC_VER)
#error "the shim builds with clang-cl (an MSVC-compatible compiler) only - issue #39"
#endif

/*	PORT_EARLY_CTOR(fn): define `static void fn(void)` and have the CRT run
	it before every normal-priority static constructor in the program.

		PORT_EARLY_CTOR(seedGuard)
		{
			...
		}

	A function pointer in the .CRT$XCT initializer slot - the CRT walks
	.CRT$XCA..XCZ in order and compiler-generated static constructors sit in
	.CRT$XCU, so XCT runs first.  Under clang-cl the pointer is marked `used`
	because an unreferenced internal global is otherwise fair game for the
	optimizer, whereas MSVC never drops __declspec(allocate) data.  */
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

/*	PORT_NORETURN: prefix for a function declaration that never returns.  */
#define PORT_NORETURN	__declspec(noreturn)

#endif
