/*	Unit test for the presenter's viewport scaling modes
	(port/psyq/vk/viewport.cpp): fit / integer / stretch over a few window
	shapes and both PS1 line counts.  Pure arithmetic - no window, no GPU.
*/
#include <cstdio>
#include <cstring>

extern "C" void Port_ViewportRect(int mode, int winW, int winH, int dispH, int out[4]);
extern "C" int  Port_ScaleModeParse(const char *s);

enum { FIT = 0, INTEGER = 1, STRETCH = 2 };

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static void expect(int mode, int winW, int winH, int dispH,
				   int x, int y, int w, int h, const char *what)
{
	int r[4];
	Port_ViewportRect(mode, winW, winH, dispH, r);
	bool ok = r[0] == x && r[1] == y && r[2] == w && r[3] == h;
	if (!ok)
		std::printf("  %s: got {%d,%d,%d,%d} want {%d,%d,%d,%d}\n",
					what, r[0], r[1], r[2], r[3], x, y, w, h);
	check(ok, what);
}

int main(void)
{
	/*	fit: the M2 letterbox  */
	expect(FIT, 1024, 768, 256,   0,   0, 1024, 768, "fit 4:3 window fills it");
	expect(FIT, 1024, 512, 256, 171,   0,  682, 512, "fit wide window pillarboxes");
	expect(FIT, 1920, 1080, 256, 240,  0, 1440, 1080, "fit 16:9");
	expect(FIT,  800, 300, 256, 200,   0,  400, 300, "fit very wide");
	expect(FIT,  640, 700, 256,   0, 110,  640, 480, "fit tall window letterboxes");

	/*	integer: whole multiples of the line count, 4:3 wide  */
	expect(INTEGER, 1920, 1080, 256, 277,  28, 1365, 1024, "integer 1080p: k=4, 1024 lines, 1365 wide");
	expect(INTEGER, 1024, 768, 256,    0,   0, 1024, 768, "integer 1024x768: k=3 exact");
	expect(INTEGER, 1024, 800, 256,    0,  16, 1024, 768, "integer 1024x800: k=3, centred");
	expect(INTEGER, 1000, 768, 256,  158,  128, 683, 512, "integer 1000 wide: k=3 too wide, k=2");
	expect(INTEGER, 1920, 1080, 240,  320,   60, 1280, 960, "integer 240-line frame: k=4");
	expect(INTEGER,  200, 100, 256,   33,    0, 133, 100, "integer too small for k=1 -> fit");
	expect(INTEGER,  342, 256, 256,    0,    0, 341, 256, "integer k=1 exactly (4:3 of 256 rounds to 341)");

	/*	stretch: the whole window  */
	expect(STRETCH, 1024, 512, 256, 0, 0, 1024, 512, "stretch = window");
	expect(STRETCH,   17,   9, 256, 0, 0,   17,   9, "stretch tiny");

	/*	degenerate input never divides by zero or goes negative  */
	int r[4];
	Port_ViewportRect(INTEGER, 0, 0, 0, r);
	check(r[2] >= 1 && r[3] >= 1, "zero window survives");

	/*	the mode words  */
	check(Port_ScaleModeParse("fit") == FIT, "parse fit");
	check(Port_ScaleModeParse("INTEGER") == INTEGER, "parse integer, any case");
	check(Port_ScaleModeParse("stretch") == STRETCH, "parse stretch");
	check(Port_ScaleModeParse("") == FIT && Port_ScaleModeParse(NULL) == FIT, "empty = fit");
	check(Port_ScaleModeParse("huge") == -1, "unknown word rejected");

	if (g_failures)
	{
		std::printf("viewport_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("viewport_test: all passed\n");
	return 0;
}
