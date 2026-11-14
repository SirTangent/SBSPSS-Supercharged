/*	vc6_sort.h - std::sort with the element order of Visual C++ 6's (issue #48).

	std::sort is not stable: elements that compare equal come out in an order
	that depends on the algorithm.  TexGrab sorts palettes by colour count and
	texture rectangles by size, and GLib's Frame sorts colours by use, with
	ties everywhere, so the order of equal elements decides where every
	palette and texture lands in VRAM - and so the bytes of every level.  The
	1999 tools were built against VC6's library, whose sort is a plain
	quicksort with no introsort fallback:
	  - while a range holds more than 16 elements, partition it around the
	    median of its first, middle and last elements (taken by value), then
	    recurse into the smaller side and loop on the larger;
	  - finish with an insertion sort over the first 16 elements, and an
	    unguarded insertion of every element after them.
	This reimplements that algorithm, not the library's text, so the rebuilt
	tools order ties as the shipped data does.
*/
#ifndef SBSP_VC6_SORT_H
#define SBSP_VC6_SORT_H

#include <algorithm>
#include <functional>
#include <iterator>

namespace vc6
{
	enum { SORT_MAX = 16 };

	template<class RI, class T, class Pr>
	void unguarded_insert(RI last, T val, Pr pred)
	{
		for (RI mid = last; pred(val, *--mid); last = mid)
			*last = *mid;
		*last = val;
	}

	template<class RI, class Pr>
	void insertion_sort(RI first, RI last, Pr pred)
	{
		typedef typename std::iterator_traits<RI>::value_type T;
		if (first == last)
			return;
		for (RI mid = first; ++mid != last; )
		{
			T val = *mid;
			if (!pred(val, *first))
				unguarded_insert(mid, val, pred);
			else
			{
				std::copy_backward(first, mid, mid + 1);
				*first = val;
			}
		}
	}

	template<class T, class Pr>
	T median(T x, T y, T z, Pr pred)
	{
		if (pred(x, y))
			return pred(y, z) ? y : pred(x, z) ? z : x;
		else if (pred(x, z))
			return x;
		else if (pred(y, z))
			return z;
		else
			return y;
	}

	template<class RI, class T, class Pr>
	RI unguarded_partition(RI first, RI last, T pivot, Pr pred)
	{
		for (; ; ++first)
		{
			for (; pred(*first, pivot); ++first)
				;
			for (; pred(pivot, *--last); )
				;
			if (last <= first)
				return first;
			std::iter_swap(first, last);
		}
	}

	template<class RI, class Pr>
	void quick(RI first, RI last, Pr pred)
	{
		typedef typename std::iterator_traits<RI>::value_type T;
		while (SORT_MAX < last - first)
		{
			RI mid = unguarded_partition(first, last,
			                             median(T(*first), T(*(first + (last - first) / 2)), T(*(last - 1)), pred),
			                             pred);
			if (last - mid <= mid - first)
			{
				quick(mid, last, pred);
				last = mid;
			}
			else
			{
				quick(first, mid, pred);
				first = mid;
			}
		}
	}

	template<class RI, class Pr>
	void sort(RI first, RI last, Pr pred)
	{
		typedef typename std::iterator_traits<RI>::value_type T;
		if (last - first <= SORT_MAX)
			insertion_sort(first, last, pred);
		else
		{
			quick(first, last, pred);
			insertion_sort(first, first + SORT_MAX, pred);
			for (first += SORT_MAX; first != last; ++first)
				unguarded_insert(first, T(*first), pred);
		}
	}

	/* operator<, called on non-const elements as VC6's less<> was (SprPal's
	   operator< is a non-const member) */
	struct less_op
	{
		template<class T> bool operator()(T &a, T &b) const { return a < b; }
	};

	template<class RI>
	void sort(RI first, RI last)
	{
		vc6::sort(first, last, less_op());	// qualified: ADL would also find std::sort
	}
}

#endif
