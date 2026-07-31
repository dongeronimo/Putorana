#include "Material.h"

namespace putorana::graphics {

// Out of line on purpose, even though it is defaulted. A polymorphic class whose
// every virtual function is inline has no key function, so the compiler emits
// the vtable and the RTTI into every translation unit that includes the header
// and leaves the linker to fold them. Defining the destructor here anchors both
// to this one object file.
Material::~Material() = default;

} // namespace putorana::graphics
