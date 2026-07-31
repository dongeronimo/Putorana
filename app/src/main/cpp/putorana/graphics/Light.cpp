#include "Light.h"

namespace putorana::graphics {

// Out of line to give the class a key function, so the vtable and RTTI are
// emitted here instead of in every TU that includes the header. Same reason as
// Material::~Material.
Light::~Light() = default;

} // namespace putorana::graphics
