// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#if defined(SFIZZ_USE_GHC_FS_FORCE)
#include <ghc/fs_fwd.hpp>
namespace fs {
using namespace ghc::filesystem;
using ifstream = ghc::filesystem::ifstream;
using ofstream = ghc::filesystem::ofstream;
using fstream = ghc::filesystem::fstream;
}
#else
#include <ghc/fs_std_fwd.hpp>
#endif
