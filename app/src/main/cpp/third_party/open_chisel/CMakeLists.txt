# OpenChisel, vendored and modified. See README.md for provenance and for the
# full list of local changes.
#
# Upstream ships a catkin package: its CMakeLists.txt hands off to catkin.cmake
# the moment it detects a ROS environment, and declares dependencies on
# roscpp/pcl_ros that do not exist here and are not needed. None of that reaches
# the library itself: the 16 translation units below depend on nothing but
# Eigen and the standard library. So the build file is replaced wholesale rather
# than patched.

# ---- Eigen ----
#
# Header-only, and the only external thing OpenChisel needs. The surface it
# actually uses is small (Vector2f/2i, Vector3f/3i, Vector4f, Matrix3f,
# Matrix4f, Quaternionf, Affine3f, aligned_allocator) which is worth knowing
# because it is also the measure of how hard this would be to port onto the GLM
# the rest of the project already uses, should the extra dependency ever start
# to hurt.
#
# Downloaded rather than vendored: it is thousands of files, and this project
# already resolves assimp and the ARCore AAR at configure time, so the network
# dependency is not a new one.
#
# FetchContent_Populate, not FetchContent_MakeAvailable: MakeAvailable would
# add_subdirectory() Eigen's own CMakeLists, which configures tests, docs,
# pkg-config and install rules we have no use for in an Android build. We want
# the headers on disk and nothing else. (The call is deprecated from CMake 3.30;
# AGP pins 3.22, where it is the documented way to do this.)
include(FetchContent)
FetchContent_Declare(
        eigen
        GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
        GIT_TAG 3.4.0
        GIT_SHALLOW TRUE
)
FetchContent_GetProperties(eigen)
if (NOT eigen_POPULATED)
    FetchContent_Populate(eigen)
endif ()

# ---- the library ----
file(GLOB_RECURSE OPEN_CHISEL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(open_chisel STATIC ${OPEN_CHISEL_SOURCES})

target_include_directories(open_chisel
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        SYSTEM PUBLIC
        ${eigen_SOURCE_DIR})

target_compile_features(open_chisel PUBLIC cxx_std_17)

# ---- optimised even in Debug ----
#
# Eigen is an expression-template library. Unoptimised, every vector operation
# stays a chain of real function calls instead of collapsing into a few
# instructions, and eigen_assert bounds-checks every access. A Debug build of the
# integrator is not "a bit slower": it is slow enough that you can no longer
# tell a real performance problem from the build configuration.
#
# This is safe on Android in a way it is NOT on Windows. There, Debug and Release
# are different C runtimes: separate heaps (free what the other allocated and you
# corrupt memory) and _ITERATOR_DEBUG_LEVEL changes the layout of std::vector, so
# the containers are not even the same type across the boundary. Android has one
# libc++ and one bionic allocator for the whole process, and optimisation level
# is not an ABI property. AGP sets no _LIBCPP_HARDENING_MODE, so libc++'s own
# inline code is identical on both sides. Mixing -O0 and -O3 objects is ordinary.
#
# The one genuine hazard is ODR, and its name is NDEBUG. Eigen turns NDEBUG into
# EIGEN_NO_DEBUG (Eigen/src/Core/util/Macros.h:987), which strips eigen_assert
# out of inline template code. Compile one Eigen header into two translation
# units with different NDEBUG and you emit two different bodies for the same weak
# symbol; the linker silently keeps one. So these flags are not optional for
# anyone who includes these headers -- hence the interface target below rather
# than a couple of PRIVATE lines.
#
# Note this also disables OpenChisel's own assert()s. That is the intended trade:
# it is vendored code we are not debugging line by line.
add_library(open_chisel_fast_flags INTERFACE)
target_compile_definitions(open_chisel_fast_flags INTERFACE NDEBUG)
target_compile_options(open_chisel_fast_flags INTERFACE $<$<CONFIG:Debug>:-O3>)

target_link_libraries(open_chisel PRIVATE open_chisel_fast_flags)

# PRIVATE above, deliberately: linking open_chisel must NOT drag -O3 and NDEBUG
# into the whole application. Instead, the ONE place that is allowed to include
# <open_chisel/...> -- the putorana::recon facade -- links open_chisel_fast_flags
# explicitly, and exposes GLM and plain structs outward so that no Eigen header
# is ever parsed by a translation unit built without these flags. See
# putorana/recon/README.md, "The boundary, and why it is also a build setting".

# Upstream is from 2014 and warns freely: unused parameters, signed/unsigned
# comparisons, reordered initialisers. Silencing them here keeps the project's
# own warnings readable; it is vendored code we are not going to police line by
# line. Anything we *did* change is called out in README.md.
target_compile_options(open_chisel PRIVATE -w)
