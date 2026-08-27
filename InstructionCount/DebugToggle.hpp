#pragma once

#include <cstdlib>

inline bool EMDebugEnabled() {
	static const bool enabled = std::getenv("EM_DEBUG") != nullptr;
	return enabled;
}
