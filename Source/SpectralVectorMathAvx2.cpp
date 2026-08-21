#include "SpectralVectorMathKernels.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
namespace smart_ducking::spectral_vector_math::detail
{
const Functions& getAvx2Functions() noexcept
{
    static const Functions functions = makeVectorFunctions<xsimd::avx2>();
    return functions;
}
}
#endif

