#ifndef RANGE_UTIL_HPP
#define RANGE_UTIL_HPP

#include <string>

// Number of elements produced by Range(start, end, step). `end` is exclusive.
inline bool rangeLength(long long start, long long end, long long step,
                        long long& count, std::string& err) {
	if (step == 0) { err = "Range step cannot be 0"; return false; }

	if (step > 0) count = start < end ? (end - start + step - 1) / step : 0;
	else          count = start > end ? (start - end + (-step) - 1) / (-step) : 0;

	if (count == 0)       { err = "Range is empty"; return false; }
	if (count > 1000000)  { err = "Range is too large (max 1,000,000 elements)"; return false; }
	return true;
}

#endif
