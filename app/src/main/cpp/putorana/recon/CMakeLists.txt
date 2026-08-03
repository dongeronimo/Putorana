# putorana::recon — the reconstruction, and the wall Eigen lives behind.
#
# Its own library rather than sources in the main one, for a reason that is not
# organisational. The TUs here are the ONLY ones in the project allowed to
# include <open_chisel/...>, and they must all be compiled with NDEBUG and -O3
# (see third_party/open_chisel/CMakeLists.txt for why: Eigen turns NDEBUG into
# EIGEN_NO_DEBUG, and compiling one Eigen header into two TUs with different
# NDEBUG emits two different bodies for the same weak symbol).
#
# A separate target is what makes that enforceable rather than a convention:
# these flags are PRIVATE to this library, and nothing it exposes outward
# mentions an Eigen type, so no translation unit built without them can ever
# parse an Eigen header.
#
# See putorana/recon/README.md, "The boundary, and why it is also a build
# setting".

add_library(putorana_recon STATIC
        Reconstruction.cpp
        Unproject.cpp)

target_include_directories(putorana_recon
        PUBLIC
        # putorana/, not cpp/: the rest of the project includes "ar/Subsystem.h"
        # and "recon/Unproject.h", so putorana/ is what those are relative to.
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        SYSTEM PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/glm)

target_compile_features(putorana_recon PUBLIC cxx_std_17)

# PRIVATE on both: open_chisel and its flags stop here. A consumer links
# putorana_recon and gets GLM and plain structs, never Eigen and never -O3.
target_link_libraries(putorana_recon
        PRIVATE
        open_chisel
        open_chisel_fast_flags)

target_link_libraries(putorana_recon PRIVATE log)
